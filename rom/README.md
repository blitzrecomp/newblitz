Put your own cartridge dump here (`.z64`, `.n64` or `.v64`; any byte order works).

Then run:

```
make rom-scan     # identify it
make rom-import   # convert to build/baserom.z64 and pin its SHA-1
```

Everything in this folder except this file is gitignored.
