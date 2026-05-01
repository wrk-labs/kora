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

  test do
    assert_match version.to_s, shell_output("#{bin}/kora --version")
  end
end
