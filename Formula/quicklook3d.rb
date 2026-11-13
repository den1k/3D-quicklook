class Quicklook3d < Formula
  desc "Quick Look previews for 3MF, STEP, STL, OBJ, glTF and PLY files"
  homepage "https://github.com/den1k/3D-quicklook"
  url "https://github.com/den1k/3D-quicklook/archive/refs/tags/v1.1.0.tar.gz"
  sha256 "0360d4d4b485a3009ddd13075370997a159685d9fab77bb17b1c0afb259b8ade"
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
