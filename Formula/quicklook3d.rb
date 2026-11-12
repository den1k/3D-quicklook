class Quicklook3d < Formula
  desc "Quick Look previews for 3MF, STEP, STL and OBJ files"
  homepage "https://github.com/den1k/3D-quicklook"
  url "https://github.com/den1k/3D-quicklook/archive/refs/tags/v1.0.0.tar.gz"
  sha256 "ce3d82f9443fb5a947f3d31c9e975faa7f1f5b69ee6fd8b1de7c4ee2b964dfe2"
  license "MIT"
  head "https://github.com/den1k/3D-quicklook.git", branch: "main"

  depends_on arch: :arm64
  depends_on :macos
  depends_on "opencascade"

  def install
    ENV["QL3D_OCCT"] = formula_opt_prefix("opencascade")
    system "./build.sh"
    prefix.install "build/QuickLook3D.app"
    pkgshare.install "scripts/install.sh"
    (bin/"quicklook3d-setup").write <<~SH
      #!/bin/bash
      exec "#{opt_pkgshare}/install.sh" "#{opt_prefix}/QuickLook3D.app"
    SH
  end

  def caveats
    <<~EOS
      To copy the app to ~/Applications and turn on the Quick Look extension, run:
        quicklook3d-setup
      Run it again after each upgrade. To remove it, delete ~/Applications/QuickLook3D.app.
    EOS
  end

  test do
    assert_path_exists prefix/"QuickLook3D.app/Contents/PlugIns/QL3DPreview.appex/Contents/MacOS/QL3DPreview"
  end
end
