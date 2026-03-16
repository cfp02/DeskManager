Put your keyboard macro text in macro.txt (one line).
At runtime the device loads /macro.txt from LittleFS; if missing, it uses the default string in code.
To upload this folder to the board's filesystem, use PlatformIO's "Upload Filesystem Image" (or pio run -t uploadfs) if your platform provides it. Otherwise flash the firmware first, then use a tool that uploads the LittleFS image built from this data/ folder.
