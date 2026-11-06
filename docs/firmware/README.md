The installer's packages land here when you run `build.bat` (or `firmware/build.sh`):
`PocketX_9NN.fwsc`, `BlackBox_9NN.fwsc` and `manifest.json`. They are published with the release and on GitHub Pages
by `PUBLISH-GITHUB.bat`, not kept in the repository.

A test build (for example `node tools/op.mjs package-test`) goes to a subfolder, `midi-test/`, with its own `manifest.json`. The
installer page offers it only through `index.html?channel=midi-test`, with a banner; the normal page and the files above are never
changed by it.
