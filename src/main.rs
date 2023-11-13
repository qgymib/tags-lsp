mod method;

use tower_lsp::lsp_types::*;

/// LSP error code that [`tower_lsp::jsonrpc::ErrorCode`] not defined.
pub enum LspErrorCode {
    /// Error code indicating that a server received a notification or
    /// request before the server has received the `initialize` request.
    ServerNotInitialized,

    /// A request failed but it was syntactically correct, e.g the
    /// method name was known and the parameters were valid. The error
    /// message should contain human readable information about why
    /// the request failed.
    RequestFailed,
}

impl LspErrorCode {
    pub const fn code(&self) -> i64 {
        match self {
            LspErrorCode::ServerNotInitialized => -32002,
            LspErrorCode::RequestFailed => -32803,
        }
    }
}

#[derive(Debug, clap::Parser)]
#[command(author, version, about, long_about = None)]
struct TagsLspArgs {
    #[arg(
        long,
        conflicts_with = "port",
        help = "Uses stdio as the communication channel"
    )]
    stdio: bool,

    #[arg(
        long,
        conflicts_with = "stdio",
        help = "Uses a socket as the communication channel"
    )]
    port: Option<i32>,

    #[arg(
        long,
        value_name = "DIR",
        help = "Specifies a directory to use for logging"
    )]
    logdir: Option<String>,

    #[arg(
        long,
        value_name = "STRING",
        help = "Set log leve.",
        long_help = "Possible values are: [OFF | TRACE | DEBUG | INFO | WARN | ERROR]. By default
`INFO` is used. Case insensitive."
    )]
    loglevel: Option<String>,
}

#[derive(Debug)]
struct Runtime {
    /// Program name.
    prog_name: String,

    /// Program version.
    prog_version: String,

    /// Workspace folder list.
    workspace_folders: Vec<WorkspaceFolder>,
}

#[derive(Debug)]
pub struct TagsLspBackend {
    client: tower_lsp::Client,
    rt: tokio::sync::Mutex<Runtime>,
}

#[tower_lsp::async_trait]
impl tower_lsp::LanguageServer for TagsLspBackend {
    async fn initialize(
        &self,
        params: InitializeParams,
    ) -> tower_lsp::jsonrpc::Result<InitializeResult> {
        return method::initialize::do_initialize(self, params).await;
    }

    async fn initialized(&self, params: InitializedParams) {
        return method::initialized::do_initialized(self, params).await;
    }

    async fn shutdown(&self) -> tower_lsp::jsonrpc::Result<()> {
        Ok(())
    }

    async fn goto_definition(
        &self,
        params: GotoDefinitionParams,
    ) -> tower_lsp::jsonrpc::Result<Option<GotoDefinitionResponse>> {
        return method::definition::goto_definition(self, params).await;
    }
}

fn setup_command_line_arguments(prog_name: &str) {
    use clap::Parser;
    let args: TagsLspArgs = TagsLspArgs::parse();

    // Get log level.
    let loglevel = match args.loglevel {
        Some(v) => v,
        None => String::from("INFO"),
    };

    // Parse log level.
    let loglevel = match loglevel.to_lowercase().as_str() {
        "off" => tracing::metadata::LevelFilter::OFF,
        "trace" => tracing::metadata::LevelFilter::TRACE,
        "debug" => tracing::metadata::LevelFilter::DEBUG,
        "info" => tracing::metadata::LevelFilter::INFO,
        "warn" => tracing::metadata::LevelFilter::WARN,
        "error" => tracing::metadata::LevelFilter::ERROR,
        unmatched => panic!(
            "Parser command line argument failed: unknown option value `{}`",
            unmatched
        ),
    };

    // Setup logging system.
    match args.logdir {
        Some(path) => {
            let logfile = format!("{}.log", prog_name);
            let file_appender = tracing_appender::rolling::never(path, logfile);
            tracing_subscriber::fmt()
                .with_max_level(loglevel)
                .with_writer(file_appender)
                .with_ansi(false)
                .init();
        }
        None => {
            tracing_subscriber::fmt()
                .with_max_level(loglevel)
                .with_writer(std::io::stderr)
                .init();
        }
    }
}

fn show_welcome(prog_name: &str, prog_version: &str) {
    tracing::info!("{} - v{}", prog_name, prog_version);
    tracing::info!("PID: {}", std::process::id());
}

#[tokio::main]
async fn main() {
    const PROG_NAME: &str = env!("CARGO_PKG_NAME");
    const PROG_VERSION: &str = env!("CARGO_PKG_VERSION");

    setup_command_line_arguments(PROG_NAME);
    show_welcome(PROG_NAME, PROG_VERSION);

    let stdin = tokio::io::stdin();
    let stdout = tokio::io::stdout();

    let rt = tokio::sync::Mutex::new(Runtime {
        prog_name: PROG_NAME.to_string(),
        prog_version: PROG_VERSION.to_string(),
        workspace_folders: Vec::new(),
    });

    let (service, socket) = tower_lsp::LspService::new(|client| TagsLspBackend { client, rt });

    tower_lsp::Server::new(stdin, stdout, socket)
        .serve(service)
        .await;
}
