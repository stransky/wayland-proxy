# Wayland proxy

Wayland proxy is load balancer between Wayland compositor and Wayland client. It prevents Wayland client to be
disconnected by Wayland compositor if Wayland client is bussy or under heavy load.

This C++ implementation is based on Rust one at https://github.com/the8472/weyland-p5000

See Mozilla Firefox bugs for details (https://bugzilla.mozilla.org/show_bug.cgi?id=1743144)

## Usage

Wayland proxy can be run as stand alone application or as a library. Stand alone application can be build
by `compile` script at `src` dir and then run Wayland application as

```
  ./wayland-proxy application_path
```

Library version can be attached to your Wayland application.
Create proxy **BEFORE** you connect app to Wayland display (usually `gtk_init()` or `wl_display_connect()` calls).

```
  // Enable logging
  WaylandProxy::SetVerbose(true);

  // Create and run Wayland proxy in extra thread
  std::unique_ptr<WaylandProxy> proxy = WaylandProxy::Create();
  if (proxy) {    
    proxy->RunThread();
  }
```

Terminate and clean up proxy:

```
  proxy = nullptr;
```

