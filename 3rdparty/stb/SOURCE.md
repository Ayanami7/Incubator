# stb_image

- Upstream: https://github.com/nothings/stb
- Pinned commit: `013ac3beddff3dbffafd5177e7972067cd2b5083` (stb_image v2.30)
- This project uses the MIT option of the upstream dual license. The original
  header and complete upstream LICENSE are retained without changes.
- `stb_image.h` SHA-256: `594c2fe35d49488b4382dbfaec8f98366defca819d916ac95becf3e75f4200b3`
- `LICENSE` SHA-256: `bebfe904b14301657e4e5d655c811d51fd31b97c455b9cc2d8600d6bac6cff63`

The project implementation is compiled once by the `stb_image` static target.
It enables PNG only, disables stdio, and limits dimensions to 4096. Runtime
links this target privately and supplies bytes through FileIO. Successful
decoding does not validate PNG CRCs or guarantee complete file integrity.
