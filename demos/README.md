## MiniOSv Demo Applications

To select and build one of the applications in this directory, run the following from the MiniOSv root directory:
```bash
# Create the app directory
mkdir app
# Copy the demo Makefile into the app directory
cp demos/Makefile app/
# Copy the demo application into the app directory
cp demos/00-hello_world.cc app/
# Build the kernel and application
make -j
```

> [!NOTE]
> The demo Makefile can be used for any demo application 

### Demo Overview

| Range | Topic |
| --- | --- |
| `00`-`09` | Introduction |
| `10`-`19` | Performance Measurements & Profiling |
