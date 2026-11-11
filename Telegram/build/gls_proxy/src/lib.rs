//! C interface to the tg-ws-proxy-rs core for Telegram GLS.
//!
//! The application loads this library at runtime, starts the proxy with the
//! same command line the standalone binary accepts and receives its events
//! through one callback. The callback runs on the proxy threads, so the
//! application copies the text and moves to its own thread.

use std::ffi::{c_char, c_void, CStr, CString};
use std::io::{self, Write};
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::sync::{Arc, Mutex, OnceLock};
use std::thread::JoinHandle;
use std::time::{Duration, Instant};

use tg_ws_proxy_rs::config::Config;
use tg_ws_proxy_rs::{default_domains, server, ws_client};
use tokio::sync::watch;
use tracing_subscriber::fmt::MakeWriter;
use tracing_subscriber::layer::SubscriberExt;
use tracing_subscriber::reload;
use tracing_subscriber::util::SubscriberInitExt;
use tracing_subscriber::{EnvFilter, Registry};

/// A log line of the running proxy
pub const EVENT_LOG: i32 = 1;
/// The listener is up, the text is the `tg://proxy` link
pub const EVENT_LISTENING: i32 = 2;
/// The proxy stopped after a stop request
pub const EVENT_STOPPED: i32 = 3;
/// The proxy could not start or failed, the text is the reason
pub const EVENT_ERROR: i32 = 4;
/// One check result: `dc<TAB>route<TAB>milliseconds<TAB>details`
pub const EVENT_CHECK: i32 = 5;
/// The check finished, the text is the number of Cloudflare domains it used
pub const EVENT_CHECK_DONE: i32 = 6;

pub const START_OK: i32 = 0;
pub const START_BAD_ARGUMENTS: i32 = 1;
pub const START_RUNNING: i32 = 2;
pub const START_FAILED: i32 = 3;

const DC_COUNT: u32 = 5;
const CHECK_TIMEOUT: Duration = Duration::from_secs(8);
const SHUTDOWN_TIMEOUT: Duration = Duration::from_secs(2);
const WORKER_THREADS: usize = 2;

pub type EventCallback = extern "C" fn(ctx: *mut c_void, kind: i32, text: *const c_char);

#[derive(Clone, Copy)]
struct Sink {
    callback: EventCallback,
    context: *mut c_void,
}

// The application keeps the context alive while the library runs and the
// callback is safe to call from any thread
unsafe impl Send for Sink {}
unsafe impl Sync for Sink {}

impl Sink {
    fn emit(&self, kind: i32, text: &str) {
        let text = CString::new(text.replace('\0', " ")).unwrap_or_default();
        (self.callback)(self.context, kind, text.as_ptr());
    }
}

struct State {
    shutdown: Option<watch::Sender<bool>>,
    thread: Option<JoinHandle<()>>,
}

static STATE: Mutex<State> = Mutex::new(State {
    shutdown: None,
    thread: None,
});
static LOG_SINK: Mutex<Option<Sink>> = Mutex::new(None);
static LOG_FILTER: OnceLock<reload::Handle<EnvFilter, Registry>> = OnceLock::new();

/// Starts the proxy with a command line in the format of the standalone binary.
///
/// # Safety
/// `args` is a NUL-terminated UTF-8 string, `context` stays valid until the
/// `EVENT_STOPPED` or `EVENT_ERROR` event of this run.
#[no_mangle]
pub unsafe extern "C" fn gls_proxy_start(
    args: *const c_char,
    callback: EventCallback,
    context: *mut c_void,
) -> i32 {
    let sink = Sink { callback, context };
    let Some(args) = read_string(args) else {
        sink.emit(EVENT_ERROR, "the command line is not valid UTF-8");
        return START_BAD_ARGUMENTS;
    };
    catch_unwind(AssertUnwindSafe(|| start(&args, sink))).unwrap_or_else(|_| {
        sink.emit(EVENT_ERROR, "the proxy failed to start");
        START_FAILED
    })
}

/// Asks the proxy to stop, waiting for it to finish when `wait` is not zero.
#[no_mangle]
pub extern "C" fn gls_proxy_stop(wait: i32) {
    let _ = catch_unwind(|| {
        let thread = {
            let Ok(mut state) = STATE.lock() else {
                return;
            };
            if let Some(shutdown) = state.shutdown.take() {
                let _ = shutdown.send(true);
            }
            if wait != 0 {
                state.thread.take()
            } else {
                None
            }
        };
        if let Some(thread) = thread {
            let _ = thread.join();
        }
    });
}

/// Returns 1 while a proxy thread exists, 0 otherwise.
#[no_mangle]
pub extern "C" fn gls_proxy_is_running() -> i32 {
    catch_unwind(|| {
        STATE
            .lock()
            .map(|state| state.thread.as_ref().is_some_and(|t| !t.is_finished()))
            .unwrap_or(false)
    })
    .map(i32::from)
    .unwrap_or(0)
}

/// Probes every data center by the routes the given command line configures.
///
/// # Safety
/// `args` is a NUL-terminated UTF-8 string, `context` stays valid until the
/// `EVENT_CHECK_DONE` event.
#[no_mangle]
pub unsafe extern "C" fn gls_proxy_check(
    args: *const c_char,
    callback: EventCallback,
    context: *mut c_void,
) -> i32 {
    let sink = Sink { callback, context };
    let Some(args) = read_string(args) else {
        sink.emit(EVENT_CHECK_DONE, "0");
        return START_BAD_ARGUMENTS;
    };
    let config = match Config::try_from_cli_line(&args) {
        Ok(config) => config,
        Err(error) => {
            sink.emit(EVENT_CHECK_DONE, "0");
            sink.emit(EVENT_ERROR, &error);
            return START_BAD_ARGUMENTS;
        }
    };
    let spawned = std::thread::Builder::new()
        .name("gls-proxy-check".into())
        .spawn(move || {
            let domains = catch_unwind(AssertUnwindSafe(|| check(&config, sink))).unwrap_or(0);
            sink.emit(EVENT_CHECK_DONE, &domains.to_string());
        });
    match spawned {
        Ok(_) => START_OK,
        Err(_) => {
            sink.emit(EVENT_CHECK_DONE, "0");
            START_FAILED
        }
    }
}

unsafe fn read_string(value: *const c_char) -> Option<String> {
    if value.is_null() {
        return None;
    }
    CStr::from_ptr(value).to_str().ok().map(str::to_owned)
}

fn start(args: &str, sink: Sink) -> i32 {
    let mut state = match STATE.lock() {
        Ok(state) => state,
        Err(_) => {
            sink.emit(EVENT_ERROR, "the proxy state is broken");
            return START_FAILED;
        }
    };
    if state.thread.as_ref().is_some_and(|t| t.is_finished()) {
        if let Some(thread) = state.thread.take() {
            let _ = thread.join();
        }
    }
    if state.thread.is_some() {
        return START_RUNNING;
    }

    let config = match Config::try_from_cli_line(args) {
        Ok(config) => config,
        Err(error) => {
            sink.emit(EVENT_ERROR, &error);
            return START_BAD_ARGUMENTS;
        }
    };
    install_logging(&config, sink);
    server::install_crypto_provider();

    let (shutdown_sender, shutdown_receiver) = watch::channel(false);
    let spawned = std::thread::Builder::new()
        .name("gls-proxy".into())
        .spawn(move || run(config, shutdown_receiver, sink));
    match spawned {
        Ok(thread) => {
            state.shutdown = Some(shutdown_sender);
            state.thread = Some(thread);
            START_OK
        }
        Err(error) => {
            sink.emit(EVENT_ERROR, &format!("failed to start a thread: {error}"));
            START_FAILED
        }
    }
}

fn run(config: Config, mut shutdown: watch::Receiver<bool>, sink: Sink) {
    let runtime = match tokio::runtime::Builder::new_multi_thread()
        .enable_all()
        .worker_threads(WORKER_THREADS)
        .thread_name("gls-proxy-worker")
        .build()
    {
        Ok(runtime) => runtime,
        Err(error) => {
            sink.emit(EVENT_ERROR, &format!("failed to start the runtime: {error}"));
            return;
        }
    };
    let result = catch_unwind(AssertUnwindSafe(|| {
        runtime.block_on(async move {
            let stopped = async move {
                while !*shutdown.borrow() {
                    if shutdown.changed().await.is_err() {
                        break;
                    }
                }
            };
            server::run_with_listen(config, stopped, |info| {
                sink.emit(EVENT_LISTENING, &info.tg_link);
            })
            .await
        })
    }));
    runtime.shutdown_timeout(SHUTDOWN_TIMEOUT);
    match result {
        Ok(Ok(())) => sink.emit(EVENT_STOPPED, ""),
        Ok(Err(error)) => sink.emit(EVENT_ERROR, &error.to_string()),
        Err(_) => sink.emit(EVENT_ERROR, "the proxy crashed"),
    }
}

fn check(config: &Config, sink: Sink) -> usize {
    let Ok(runtime) = tokio::runtime::Builder::new_multi_thread()
        .enable_all()
        .worker_threads(WORKER_THREADS)
        .build()
    else {
        return 0;
    };
    runtime.block_on(async {
        let Ok(outbound) = config.outbound_connector() else {
            return 0;
        };
        let outbound = Arc::new(outbound);
        let mut domains = config.cf_domains.clone();
        if config.default_domains {
            for domain in default_domains::fetch_default_domains_with_outbound(&outbound).await {
                if !domains.contains(&domain) {
                    domains.push(domain);
                }
            }
        }
        let mut probes = Vec::new();
        for dc in 1..=DC_COUNT {
            let ip = config.dc_target_ip(dc).map(str::to_owned);
            let domains = domains.clone();
            let outbound = Arc::clone(&outbound);
            let skip_tls = config.skip_tls_verify;
            let fronting = config.fronting_domain.clone();
            probes.push(tokio::spawn(async move {
                probe(dc, ip, &domains, &outbound, skip_tls, fronting.as_deref()).await
            }));
        }
        for (index, probe) in probes.into_iter().enumerate() {
            let line = probe
                .await
                .unwrap_or_else(|_| format!("{}\tfail\t0\tprobe crashed", index + 1));
            sink.emit(EVENT_CHECK, &line);
        }
        domains.len()
    })
}

async fn probe(
    dc: u32,
    ip: Option<String>,
    domains: &[String],
    outbound: &tg_ws_proxy_rs::outbound::OutboundConnector,
    skip_tls: bool,
    fronting: Option<&str>,
) -> String {
    let mut details = String::new();
    if let Some(ip) = &ip {
        let started = Instant::now();
        let attempt = ws_client::connect_ws_for_dc_with_outbound(
            ip,
            dc,
            false,
            skip_tls,
            CHECK_TIMEOUT,
            outbound,
            fronting,
        )
        .await;
        if attempt.ws.is_some() {
            return format!("{dc}\tws\t{}\t{ip}", started.elapsed().as_millis());
        }
        details = if attempt.all_redirects {
            "websocket redirected".into()
        } else if attempt.connect_timed_out {
            "websocket connect timed out".into()
        } else if attempt.upgrade_timed_out {
            "websocket handshake timed out".into()
        } else {
            "websocket failed".into()
        };
    }
    if !domains.is_empty() {
        let started = Instant::now();
        let (stream, record, _) = ws_client::connect_cf_ws_for_dc_with_outbound(
            dc,
            domains,
            false,
            skip_tls,
            CHECK_TIMEOUT,
            outbound,
        )
        .await;
        if stream.is_some() {
            return format!(
                "{dc}\tcf\t{}\t{}",
                started.elapsed().as_millis(),
                record.unwrap_or_default()
            );
        }
        if !details.is_empty() {
            details.push_str(", ");
        }
        details.push_str("cloudflare failed");
    }
    if details.is_empty() {
        details = "no route configured".into();
    }
    format!("{dc}\tfail\t0\t{details}")
}

fn install_logging(config: &Config, sink: Sink) {
    if let Ok(mut current) = LOG_SINK.lock() {
        *current = Some(sink);
    }
    let level = if config.quiet {
        "off"
    } else if config.verbose {
        "debug"
    } else {
        "info"
    };
    if let Some(handle) = LOG_FILTER.get() {
        let _ = handle.reload(EnvFilter::new(level));
        return;
    }
    let (filter, handle) = reload::Layer::new(EnvFilter::new(level));
    let installed = Registry::default()
        .with(filter)
        .with(
            tracing_subscriber::fmt::layer()
                .with_ansi(false)
                .with_target(false)
                .without_time()
                .with_writer(SinkWriterMaker),
        )
        .try_init()
        .is_ok();
    if installed {
        let _ = LOG_FILTER.set(handle);
    }
}

struct SinkWriterMaker;

impl<'a> MakeWriter<'a> for SinkWriterMaker {
    type Writer = SinkWriter;

    fn make_writer(&'a self) -> Self::Writer {
        SinkWriter { buffer: Vec::new() }
    }
}

struct SinkWriter {
    buffer: Vec<u8>,
}

impl SinkWriter {
    fn forward(line: &[u8]) {
        let line = String::from_utf8_lossy(line);
        let line = line.trim_end();
        if line.is_empty() {
            return;
        }
        let sink = LOG_SINK.lock().ok().and_then(|sink| *sink);
        if let Some(sink) = sink {
            sink.emit(EVENT_LOG, line);
        }
    }
}

impl Write for SinkWriter {
    fn write(&mut self, data: &[u8]) -> io::Result<usize> {
        self.buffer.extend_from_slice(data);
        while let Some(position) = self.buffer.iter().position(|&b| b == b'\n') {
            let line: Vec<u8> = self.buffer.drain(..=position).collect();
            Self::forward(&line);
        }
        Ok(data.len())
    }

    fn flush(&mut self) -> io::Result<()> {
        if !self.buffer.is_empty() {
            let line = std::mem::take(&mut self.buffer);
            Self::forward(&line);
        }
        Ok(())
    }
}

impl Drop for SinkWriter {
    fn drop(&mut self) {
        let _ = self.flush();
    }
}
