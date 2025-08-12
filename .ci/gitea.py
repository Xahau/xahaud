#!/usr/bin/env python3
"""
Persistent Gitea for Conan on Self-Hosted GA Runner
- Localhost only (127.0.0.1) for security
- Persistent volumes survive between workflows
- Idempotent - safe to run multiple times
- Reuses existing container if already running
- Uses pre-baked app.ini to bypass web setup wizard
"""

import argparse
import logging
import os
import queue
import shutil
import socket
import subprocess
import sys
import threading
import time
from typing import Optional


class DockerLogStreamer(threading.Thread):
    """Background thread to stream docker logs -f and pass lines into a queue"""

    def __init__(self, container_name: str, log_queue: queue.Queue):
        super().__init__(name=f"DockerLogStreamer-{container_name}")
        self.container = container_name
        self.log_queue = log_queue
        self._stop_event = threading.Event()
        self.proc: Optional[subprocess.Popen] = None
        self.daemon = True  # so it won't block interpreter exit if something goes wrong

    def run(self):
        try:
            # Follow logs, capture both stdout and stderr
            self.proc = subprocess.Popen(
                ["docker", "logs", "-f", self.container],
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                bufsize=1,
                universal_newlines=True,
            )
            if not self.proc.stdout:
                return
            for line in self.proc.stdout:
                if line is None:
                    break
                # Ensure exact line fidelity
                self.log_queue.put(line.rstrip("\n"))
                if self._stop_event.is_set():
                    break
        except Exception as e:
            # Put an error marker so consumer can see
            self.log_queue.put(f"[STREAMER_ERROR] {e}")
        finally:
            try:
                if self.proc and self.proc.poll() is None:
                    # Do not kill abruptly unless asked to stop
                    pass
            except Exception:
                pass

    def stop(self, timeout: float = 5.0):
        self._stop_event.set()
        try:
            if self.proc and self.proc.poll() is None:
                # Politely terminate docker logs
                self.proc.terminate()
                try:
                    self.proc.wait(timeout=timeout)
                except Exception:
                    self.proc.kill()
        except Exception:
            pass


class PersistentGiteaConan:
    def __init__(self, debug: bool = False, verbose: bool = False):
        # Configurable via environment variables for CI flexibility
        self.container = os.getenv("GITEA_CONTAINER_NAME", "gitea-conan-persistent")
        self.port = int(os.getenv("GITEA_PORT", "3000"))
        self.user = os.getenv("GITEA_USER", "conan")
        self.passwd = os.getenv("GITEA_PASSWORD", "conan-pass-2024")  # do not print this in logs
        self.email = os.getenv("GITEA_EMAIL", "conan@localhost")
        # Persistent data location on the runner
        self.data_dir = os.getenv("GITEA_DATA_DIR", "/opt/gitea")
        # Behavior flags
        self.print_credentials = os.getenv("GITEA_PRINT_CREDENTIALS", "0") == "1"
        self.startup_timeout = int(os.getenv("GITEA_STARTUP_TIMEOUT", "120"))

        # Logging and docker log streaming infrastructure
        self._setup_logging(debug=debug, verbose=verbose)
        self.log_queue: queue.Queue[str] = queue.Queue()
        self.log_streamer: Optional[DockerLogStreamer] = None
        # Conan execution context cache
        self._conan_prefix: Optional[str] = None  # '' for direct, full sudo+shell for delegated; None if unavailable

    def _setup_logging(self, debug: bool, verbose: bool):
        # Determine level: debug > verbose > default WARNING
        if debug:
            level = logging.DEBUG
        elif verbose:
            level = logging.INFO
        else:
            level = logging.WARNING
        logging.basicConfig(level=level, format='%(asctime)s - %(levelname)s - %(filename)s:%(lineno)d - %(message)s')
        self.logger = logging.getLogger(__name__)
        # Be slightly quieter for noisy libs
        logging.getLogger('urllib3').setLevel(logging.WARNING)

    def run(self, cmd, check=True, env=None):
        """Run command with minimal output"""
        run_env = os.environ.copy()
        if env:
            run_env.update(env)
        self.logger.debug(f"EXEC: {cmd}")
        result = subprocess.run(cmd, shell=True, capture_output=True, text=True, env=run_env)
        if result.stdout:
            self.logger.debug(f"STDOUT: {result.stdout.strip()}"[:1000])
        if result.stderr:
            self.logger.debug(f"STDERR: {result.stderr.strip()}"[:1000])
        if result.returncode != 0 and check:
            self.logger.error(f"Command failed ({result.returncode}) for: {cmd}")
            raise RuntimeError(f"Command failed: {result.stderr}")
        return result

    def is_running(self):
        """Check if container is already running"""
        result = self.run(f"docker ps -q -f name={self.container}", check=False)
        return bool(result.stdout.strip())

    def container_exists(self):
        """Check if container exists (running or stopped)"""
        result = self.run(f"docker ps -aq -f name={self.container}", check=False)
        return bool(result.stdout.strip())

    # ---------- Helpers & Preflight Checks ----------

    def _check_docker(self):
        if not shutil.which("docker"):
            raise RuntimeError(
                "Docker is not installed or not in PATH. Please install Docker and ensure the daemon is running.")
        # Check daemon access
        info = subprocess.run("docker info", shell=True, capture_output=True, text=True)
        if info.returncode != 0:
            raise RuntimeError(
                "Docker daemon not accessible. Ensure the Docker service is running and the current user has permission to use Docker.")

    def _is_port_in_use(self, host, port):
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            s.settimeout(0.5)
            return s.connect_ex((host, port)) == 0

    def _setup_directories(self):
        """Create directory structure with proper ownership"""
        gitea_data = os.path.join(self.data_dir, "gitea")
        gitea_conf = os.path.join(gitea_data, "gitea", "conf")

        # Create all directories
        os.makedirs(gitea_conf, exist_ok=True)
        self.logger.info(f"📁 Created directory structure: {self.data_dir}")

        # Set ownership recursively to git user (UID 1000)
        for root, dirs, files in os.walk(self.data_dir):
            os.chown(root, 1000, 1000)
            for d in dirs:
                os.chown(os.path.join(root, d), 1000, 1000)
            for f in files:
                os.chown(os.path.join(root, f), 1000, 1000)

    def _preflight(self):
        self.logger.info("🔍 Running preflight checks...")
        self._check_docker()
        # Port check only if our container is not already running
        if not self.is_running():
            if self._is_port_in_use("127.0.0.1", self.port):
                raise RuntimeError(f"Port {self.port} on 127.0.0.1 is already in use. Cannot bind Gitea.")
        self.logger.info("✓ Preflight checks passed")

    def _generate_secret(self, secret_type):
        """Generate a secret using Gitea's built-in generator"""
        cmd = f"docker run --rm gitea/gitea:latest gitea generate secret {secret_type}"
        result = self.run(cmd)
        return result.stdout.strip()

    def _create_app_ini(self, gitea_conf_dir):
        """Create a pre-configured app.ini file"""
        app_ini_path = os.path.join(gitea_conf_dir, "app.ini")

        # Check if app.ini already exists (from previous run)
        if os.path.exists(app_ini_path):
            self.logger.info("✓ Using existing app.ini configuration")
            # Minimal migration: ensure HTTP_ADDR allows inbound connections from host via Docker mapping
            try:
                with open(app_ini_path, 'r+', encoding='utf-8') as f:
                    content = f.read()
                    updated = False
                    if "HTTP_ADDR = 127.0.0.1" in content:
                        content = content.replace("HTTP_ADDR = 127.0.0.1", "HTTP_ADDR = 0.0.0.0")
                        updated = True
                    if updated:
                        f.seek(0)
                        f.write(content)
                        f.truncate()
                        self.logger.info("🔁 Updated existing app.ini to bind on 0.0.0.0 for container reachability")
            except Exception as e:
                self.logger.warning(f"⚠️  Could not update existing app.ini automatically: {e}")
            return

        self.logger.info("🔑 Generating security secrets...")
        secret_key = self._generate_secret("SECRET_KEY")
        internal_token = self._generate_secret("INTERNAL_TOKEN")

        self.logger.info("📝 Creating app.ini configuration...")
        app_ini_content = f"""APP_NAME = Conan Package Registry
RUN_USER = git
RUN_MODE = prod

[server]
ROOT_URL = http://127.0.0.1:{self.port}/
HTTP_ADDR = 0.0.0.0
HTTP_PORT = 3000
DISABLE_SSH = true
START_SSH_SERVER = false
OFFLINE_MODE = true
DOMAIN = localhost
LFS_START_SERVER = false

[database]
DB_TYPE = sqlite3
PATH = /data/gitea.db
LOG_SQL = false

[repository]
ROOT = /data/gitea-repositories
DISABLED_REPO_UNITS = repo.issues, repo.pulls, repo.wiki, repo.projects, repo.actions

[security]
INSTALL_LOCK = true
SECRET_KEY = {secret_key}
INTERNAL_TOKEN = {internal_token}
PASSWORD_HASH_ALGO = pbkdf2
MIN_PASSWORD_LENGTH = 8

[service]
DISABLE_REGISTRATION = true
ENABLE_NOTIFY_MAIL = false
REGISTER_EMAIL_CONFIRM = false
ENABLE_CAPTCHA = false
REQUIRE_SIGNIN_VIEW = false
DEFAULT_KEEP_EMAIL_PRIVATE = true
DEFAULT_ALLOW_CREATE_ORGANIZATION = false
DEFAULT_ENABLE_TIMETRACKING = false

[mailer]
ENABLED = false

[session]
PROVIDER = file

[log]
MODE = console
LEVEL = Info

[api]
ENABLE_SWAGGER = false

[packages]
ENABLED = true

[other]
SHOW_FOOTER_VERSION = false
"""

        # Write app.ini with restrictive permissions
        with open(app_ini_path, 'w') as f:
            f.write(app_ini_content)

        # Set ownership to UID 1000:1000 (git user in container)
        os.chown(app_ini_path, 1000, 1000)
        os.chmod(app_ini_path, 0o640)

        self.logger.info("✓ Created app.ini with pre-generated secrets")

    def setup(self):
        """Setup or verify Gitea is running"""
        self.logger.info("🔧 Setting up persistent Gitea for Conan...")
        # Preflight
        self._preflight()

        # Create persistent data directory structure with proper ownership
        self._setup_directories()
        gitea_data = os.path.join(self.data_dir, "gitea")
        gitea_conf = os.path.join(gitea_data, "gitea", "conf")

        # Create app.ini BEFORE starting container (for headless setup)
        self._create_app_ini(gitea_conf)

        # Check if already running
        if self.is_running():
            self.logger.info("✅ Gitea container already running")
            self._verify_health()
            self._configure_conan()
            return

        # Check if container exists but stopped
        if self.container_exists():
            self.logger.info("🔄 Starting existing container...")
            self.run(f"docker start {self.container}")
            # Start log streaming for visibility
            self._start_log_streaming()
            try:
                time.sleep(2)
                self._verify_health()
                self._configure_conan()
            finally:
                self._stop_log_streaming()
            return

        # Create new container (first time setup)
        self.logger.info("🚀 Creating new Gitea container...")

        gitea_data = os.path.join(self.data_dir, "gitea")

        # IMPORTANT: Bind to 127.0.0.1 only for security
        # With pre-configured app.ini, Gitea starts directly without wizard
        docker_cmd = f"""docker run -d \
            --name {self.container} \
            -p 127.0.0.1:{self.port}:3000 \
            -v {gitea_data}:/data \
            -v /etc/timezone:/etc/timezone:ro \
            -v /etc/localtime:/etc/localtime:ro \
            -e USER_UID=1000 \
            -e USER_GID=1000 \
            --restart unless-stopped \
            gitea/gitea:latest"""

        self.run(docker_cmd)

        # Debug: Check actual port mapping
        port_check = self.run(f"docker port {self.container}", check=False)
        self.logger.info(f"🔍 Container port mapping: {port_check.stdout.strip()}")

        # Start log streaming and wait for Gitea to be ready
        self._start_log_streaming()
        try:
            self._wait_for_startup(self.startup_timeout)
            # Create user (idempotent)
            self._create_user()
            # Configure Conan
            self._configure_conan()
        finally:
            self._stop_log_streaming()

        self.logger.info("✅ Persistent Gitea ready for Conan packages!")
        self.logger.info(f"   URL: http://localhost:{self.port}")
        if self.print_credentials:
            self.logger.info(f"   User: {self.user} / {self.passwd}")
        else:
            self.logger.info("   Credentials: hidden (set GITEA_PRINT_CREDENTIALS=1 to display)")
        self.logger.info(f"   Data persisted in: {self.data_dir}")

    def _start_log_streaming(self):
        # Start background docker log streamer if not running
        if self.log_streamer is not None:
            self._stop_log_streaming()
        self.logger.debug("Starting Docker log streamer...")
        self.log_streamer = DockerLogStreamer(self.container, self.log_queue)
        self.log_streamer.start()

    def _stop_log_streaming(self):
        if self.log_streamer is not None:
            self.logger.debug("Stopping Docker log streamer...")
            try:
                self.log_streamer.stop()
                self.log_streamer.join(timeout=5)
            except Exception:
                pass
            finally:
                self.log_streamer = None

    def _wait_for_startup(self, timeout=60):
        """Wait for container to become healthy by consuming the docker log stream"""
        self.logger.info(f"⏳ Waiting for Gitea to start (timeout: {timeout}s)...")
        start_time = time.time()
        server_detected = False

        while time.time() - start_time < timeout:
            # Drain all available log lines without blocking
            drained_any = False
            while True:
                try:
                    line = self.log_queue.get_nowait()
                    drained_any = True
                except queue.Empty:
                    break
                if not line.strip():
                    continue
                # Always log raw docker lines at DEBUG level
                self.logger.debug(f"DOCKER: {line}")
                # Promote important events
                l = line
                if ("[E]" in l) or ("ERROR" in l) or ("FATAL" in l) or ("panic" in l):
                    self.logger.error(l)
                elif ("WARN" in l) or ("[W]" in l):
                    self.logger.warning(l)
                # Detect startup listening lines
                if ("Web server is now listening" in l) or ("Listen:" in l) or ("Starting new Web server" in l):
                    if not server_detected:
                        server_detected = True
                        self.logger.info("✓ Detected web server startup!")
                        self.logger.info("⏳ Waiting for Gitea to fully initialize...")
                        # Quick readiness loop
                        for i in range(10):
                            time.sleep(1)
                            if self._is_healthy():
                                self.logger.info(f"✓ Gitea is ready and responding! (after {i + 1} seconds)")
                                return
                        self.logger.warning(
                            "Server started but health check failed after 10 attempts, continuing to wait...")

            # Check if container is still running periodically
            container_status = self.run(
                f"docker inspect {self.container} --format='{{{{.State.Status}}}}'",
                check=False
            )
            status = (container_status.stdout or "").strip()
            if status and status != "running":
                # Container stopped or in error state
                error_logs = self.run(
                    f"docker logs --tail 30 {self.container} 2>&1",
                    check=False
                )
                self.logger.error(f"Container is in '{status}' state. Last logs:")
                for l in (error_logs.stdout or "").split('\n')[-10:]:
                    if l.strip():
                        self.logger.error(l)
                raise RuntimeError(f"Container failed to start (status: {status})")

            # If nothing drained, brief sleep to avoid busy loop
            if not drained_any:
                time.sleep(0.5)

        raise TimeoutError(f"Gitea failed to become ready within {timeout} seconds")

    def _is_healthy(self):
        """Check if Gitea is responding"""
        # Try a simple HTTP GET first (less verbose)
        result = self.run(
            f"curl -s -o /dev/null -w '%{{http_code}}' http://localhost:{self.port}/",
            check=False
        )
        code = result.stdout.strip()
        # Treat any 2xx/3xx as healthy (e.g., 200 OK, 302/303 redirects)
        if code and code[0] in ("2", "3"):
            return True

        # If it failed, show debug info
        if code == "000":
            # Only show debug on first failure
            if not hasattr(self, '_health_check_debug_shown'):
                self._health_check_debug_shown = True
                self.logger.info("🔍 Connection issue detected, showing diagnostics:")

                # Check what's actually listening
                netstat_result = self.run(f"netstat -tln | grep {self.port}", check=False)
                self.logger.info(f"    Port {self.port} listeners: {netstat_result.stdout.strip() or 'none found'}")

                # Check docker port mapping
                port_result = self.run(f"docker port {self.container} 3000", check=False)
                self.logger.info(f"    Docker mapping: {port_result.stdout.strip() or 'not mapped'}")

        return False

    def _verify_health(self):
        """Verify Gitea is healthy"""
        if not self._is_healthy():
            raise RuntimeError("Gitea is not responding properly")
        self.logger.info("✅ Gitea is healthy")

    # ---------- Conan helpers ----------
    def _resolve_conan_prefix(self) -> Optional[str]:
        """Determine how to run the 'conan' CLI and cache the decision.
        Returns:
          '' for direct invocation (conan in PATH),
          full sudo+login-shell prefix string for delegated execution, or
          None if Conan is not available.
        """
        if self._conan_prefix is not None:
            return self._conan_prefix

        # If running with sudo, try actual user's login shell
        if os.geteuid() == 0 and 'SUDO_USER' in os.environ:
            actual_user = os.environ['SUDO_USER']
            # Discover the user's shell
            shell_result = self.run(f"getent passwd {actual_user} | cut -d: -f7", check=False)
            user_shell = shell_result.stdout.strip() if shell_result.returncode == 0 and shell_result.stdout.strip() else "/bin/bash"
            self.logger.info(f"→ Using {actual_user}'s shell for Conan: {user_shell}")

            which_result = self.run(f"sudo -u {actual_user} {user_shell} -l -c 'which conan'", check=False)
            if which_result.returncode == 0 and which_result.stdout.strip():
                self._conan_prefix = f"sudo -u {actual_user} {user_shell} -l -c"
                self.logger.info(f"✓ Found Conan at: {which_result.stdout.strip()}")
                return self._conan_prefix
            else:
                self.logger.warning(f"⚠️  Conan not found in {actual_user}'s PATH.")
                self._conan_prefix = None
                return self._conan_prefix
        else:
            # Non-sudo case; check PATH directly
            if shutil.which("conan"):
                self._conan_prefix = ''
                return self._conan_prefix
            else:
                self.logger.warning("⚠️  Conan CLI not found in PATH.")
                self._conan_prefix = None
                return self._conan_prefix

    def _build_conan_cmd(self, inner_args: str) -> Optional[str]:
        """Build a shell command to run Conan with given inner arguments.
        Example: inner_args='remote list' => 'conan remote list' or "sudo -u user shell -l -c 'conan remote list'".
        Returns None if Conan is unavailable.
        """
        prefix = self._resolve_conan_prefix()
        if prefix is None:
            return None
        if prefix == '':
            return f"conan {inner_args}"
        # Delegate via sudo+login shell; quote the inner command
        return f"{prefix} 'conan {inner_args}'"

    def _run_conan(self, inner_args: str, check: bool = False):
        """Run a Conan subcommand using the resolved execution context.
        Returns the subprocess.CompletedProcess-like result, or a dummy object with returncode=127 if unavailable.
        """
        full_cmd = self._build_conan_cmd(inner_args)
        if full_cmd is None:
            # Construct a minimal dummy result
            class Dummy:
                returncode = 127
                stdout = ''
                stderr = 'conan: not found'
            self.logger.error("❌ Conan CLI is not available. Skipping command: conan " + inner_args)
            return Dummy()
        return self.run(full_cmd, check=check)

    def _create_user(self):
        """Create Conan user (idempotent)"""
        self.logger.info("👤 Setting up admin user...")

        # Retry a few times in case DB initialization lags behind
        attempts = 5
        for i in range(1, attempts + 1):
            # First check if user exists
            check_cmd = f"docker exec -u 1000:1000 {self.container} gitea admin user list"
            result = self.run(check_cmd, check=False)
            if result.returncode == 0 and self.user in result.stdout:
                self.logger.info(f"✅ User already exists: {self.user}")
                return

            # Try to create admin user with --admin flag
            create_cmd = f"""docker exec -u 1000:1000 {self.container} \
                gitea admin user create \
                --username {self.user} \
                --password {self.passwd} \
                --email {self.email} \
                --admin \
                --must-change-password=false"""
            create_res = self.run(create_cmd, check=False)
            if create_res.returncode == 0:
                self.logger.info(f"✅ Created admin user: {self.user}")
                return
            if "already exists" in (create_res.stderr or "").lower() or "already exists" in (
                    create_res.stdout or "").lower():
                self.logger.info(f"✅ User already exists: {self.user}")
                return

            if i < attempts:
                delay = min(2 ** i, 10)
                time.sleep(delay)

        self.logger.warning(f"⚠️  Could not create user after {attempts} attempts. You may need to create it manually.")

    def _configure_conan(self):
        """Configure Conan client (idempotent)"""
        self.logger.info("🔧 Configuring Conan client...")

        # Ensure Conan is available and determine execution context
        if self._resolve_conan_prefix() is None:
            self.logger.warning("⚠️  Conan CLI not available. Skipping client configuration.")
            return

        # Gitea Conan URL
        conan_url = f"http://localhost:{self.port}/api/packages/{self.user}/conan"

        # Remove old remote if exists (ignore errors)
        self._run_conan("remote remove gitea-local 2>/dev/null", check=False)

        # Add Gitea as remote (localhost only)
        self._run_conan(f"remote add gitea-local {conan_url}")

        # Authenticate (Conan masks password in process list)
        self._run_conan(f"user -p {self.passwd} -r gitea-local {self.user}")

        # Enable revisions if not already
        self._run_conan("config set general.revisions_enabled=1", check=False)

        self.logger.info(f"✅ Conan configured with remote: gitea-local")

    def verify(self):
        """Verify everything is working"""
        self.logger.info("🔍 Verifying setup...")

        # Check container
        if not self.is_running():
            self.logger.error("❌ Container not running")
            return False

        # Check Gitea health
        if not self._is_healthy():
            self.logger.error("❌ Gitea not responding")
            return False

        # Check Conan remote
        result = self._run_conan("remote list", check=False)
        if getattr(result, 'returncode', 1) != 0 or "gitea-local" not in (getattr(result, 'stdout', '') or ''):
            self.logger.error("❌ Conan remote not configured")
            return False

        self.logger.info("✅ All systems operational")
        return True

    def info(self):
        """Print current status"""
        self.logger.info("📊 Gitea Status:")
        self.logger.info(f"  Container: {self.container}")
        self.logger.info(f"  Running: {self.is_running()}")
        self.logger.info(f"  Data dir: {self.data_dir}")
        self.logger.info(f"  URL: http://localhost:{self.port}")
        self.logger.info(f"  Conan URL: http://localhost:{self.port}/api/packages/{self.user}/conan")

        # Show disk usage
        if os.path.exists(self.data_dir):
            result = self.run(f"du -sh {self.data_dir}", check=False)
            if result.returncode == 0:
                size = result.stdout.strip().split('\t')[0]
                self.logger.info(f"  Disk usage: {size}")

    def test(self):
        """Test Conan package upload/download"""
        self.logger.info("🧪 Testing Conan with Gitea...")

        # Ensure everything is set up
        if not self.is_running():
            self.logger.error("❌ Gitea not running. Run 'setup' first.")
            return False

        # Test package name
        test_package = "zlib/1.3.1"

        self.logger.info(f"  → Testing with package: {test_package}")

        # Ensure Conan execution context is resolved
        if self._resolve_conan_prefix() is None:
            self.logger.error("❌ Conan CLI not available. Cannot run test.")
            return False

        # Remove any existing package
        self.logger.info(f"  → Cleaning local cache...")
        self._run_conan(f"remove '{test_package}' -f", check=False)

        # Install and build from source
        self.logger.info(f"  → Installing {test_package} from Conan Center...")
        result = self._run_conan(f"install {test_package}@ --build={test_package}", check=False)
        if result.returncode != 0:
            self.logger.error(f"❌ Failed to install package")
            return False

        # Upload to Gitea
        self.logger.info(f"  → Uploading to Gitea...")
        result = self._run_conan(f"upload '{test_package}' --all -r gitea-local --confirm", check=False)
        if result.returncode != 0:
            self.logger.error(f"❌ Failed to upload package")
            return False

        # Remove local copy
        self.logger.info(f"  → Removing local copy...")
        self._run_conan(f"remove '{test_package}' -f", check=False)

        # Download from Gitea
        self.logger.info(f"  → Downloading from Gitea...")
        result = self._run_conan(f"install {test_package}@ -r gitea-local", check=False)
        if result.returncode == 0:
            self.logger.info(f"✅ Test successful! Package uploaded and downloaded from Gitea.")
            return True
        else:
            self.logger.error(f"❌ Failed to download from Gitea")
            return False

    def teardown(self):
        """Stop and remove Gitea container and data"""
        self.logger.info("🛑 Tearing down Gitea...")

        # Stop and remove container
        if self.container_exists():
            self.logger.info(f"  → Stopping container: {self.container}")
            self.run(f"docker stop {self.container}", check=False)
            self.logger.info(f"  → Removing container: {self.container}")
            self.run(f"docker rm {self.container}", check=False)
        else:
            self.logger.info("  → No container to remove")

        # Remove data directory
        if os.path.exists(self.data_dir):
            self.logger.info(f"  → Removing data directory: {self.data_dir}")
            shutil.rmtree(self.data_dir)
            self.logger.info("  ✓ Data directory removed")
        else:
            self.logger.info("  → No data directory to remove")

        self.logger.info("  ✅ Teardown complete!")


# For use in GitHub Actions workflows

def main():
    parser = argparse.ArgumentParser(description='Persistent Gitea for Conan packages')
    parser.add_argument('command', choices=['setup', 'teardown', 'verify', 'info', 'test'], nargs='?', default='setup')
    parser.add_argument('--debug', action='store_true', help='Enable debug logging')
    parser.add_argument('--verbose', action='store_true', help='Enable verbose logging (info level)')
    args = parser.parse_args()

    # Temporary logging before instance creation (level will be reconfigured inside class)
    temp_level = logging.DEBUG if args.debug else (logging.INFO if args.verbose else logging.WARNING)
    logging.basicConfig(level=temp_level, format='%(asctime)s - %(levelname)s - %(filename)s:%(lineno)d - %(message)s')
    logger = logging.getLogger(__name__)

    # Auto-escalate to sudo for operations that need it
    needs_sudo = args.command in ['setup', 'teardown']
    if needs_sudo and os.geteuid() != 0:
        logger.info("📋 This operation requires sudo privileges. Re-running with sudo...")
        os.execvp('sudo', ['sudo'] + sys.argv)

    gitea = PersistentGiteaConan(debug=args.debug, verbose=args.verbose)

    try:
        if args.command == "setup":
            gitea.setup()
        elif args.command == "verify":
            sys.exit(0 if gitea.verify() else 1)
        elif args.command == "info":
            gitea.info()
        elif args.command == "test":
            sys.exit(0 if gitea.test() else 1)
        elif args.command == "teardown":
            gitea.teardown()
    except KeyboardInterrupt:
        logger.warning("Interrupted by user. Cleaning up...")
        try:
            gitea._stop_log_streaming()
        except Exception:
            pass
        sys.exit(130)


if __name__ == "__main__":
    main()
