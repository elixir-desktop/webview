#include "config.hpp"
#include "host_controller.hpp"

#include <gtk/gtk.h>

#include <cstdio>
#include <memory>

int main(int argc, char** argv) {
  // Prefer software rendering when unset — WebKitGPU/DMA-BUF crashes are common on Xvfb.
  if (!g_getenv("WEBKIT_DISABLE_COMPOSITING_MODE"))
    g_setenv("WEBKIT_DISABLE_COMPOSITING_MODE", "1", FALSE);
  if (!g_getenv("WEBKIT_DISABLE_DMABUF_RENDERER"))
    g_setenv("WEBKIT_DISABLE_DMABUF_RENDERER", "1", FALSE);

  auto config = HostConfig::parse(argc, argv);

  gtk_init();

  auto host = std::make_unique<HostController>(std::move(config));
  if (!host->start()) {
    fprintf(stderr, "failed to start host\n");
    return 1;
  }

  GMainLoop* loop = g_main_loop_new(nullptr, FALSE);
  g_main_loop_run(loop);
  g_main_loop_unref(loop);
  return 0;
}
