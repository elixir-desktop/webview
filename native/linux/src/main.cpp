#include "config.hpp"
#include "host_controller.hpp"

#include <gtk/gtk.h>

#include <cstdio>
#include <memory>

int main(int argc, char** argv) {
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
