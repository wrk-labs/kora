class Kora < Formula
  desc "Terminal-based local LLM chat client"
  homepage "https://wrklabs.org/kora"
  version "__VERSION__"
  license "GPL-2.0-only"

  depends_on arch: :arm64
  depends_on :macos
  depends_on "ncurses"

  url "https://brew.wrklabs.org/dist/kora/kora-__VERSION__-darwin-arm64.tar.gz"
  sha256 "__SHA_ARM64__"

  def install
    bin.install "kora", "llama-server"
    (share/"kora/lua/core").install Dir["lua/core/*.lua"]
  end

  service do
    run [opt_bin/"kora", "serve"]
    run_at_load true
    keep_alive successful_exit: false
    watch_paths ["#{ENV["HOME"]}/.kora/preferred_model"]
    log_path var/"log/kora.log"
    error_log_path var/"log/kora.log"
  end

  def caveats
    <<~EOS
      kora ships with a launchd agent that runs `kora serve` in the background.
      Start it once and macOS will keep it running across logins:

        brew services start kora

      The server stays idle until you pull a model:

        kora pull <model>

      It picks up the model automatically — no need to restart the service.
    EOS
  end

  test do
    assert_match version.to_s, shell_output("#{bin}/kora --version")
  end
end
