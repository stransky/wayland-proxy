/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "wayland-proxy.h"
#include <cstdio>

int main(int argc, char* argv[]) {
  if (argc < 2) {
    printf("Wayland proxy load balancer, run as:\n\n%s application_name\n\n", argv[0]);
    return 0;
  }
  WaylandProxy::SetVerbose(true);
  auto proxy = WaylandProxy::Create();
  if (!proxy) {
    return 1;
  }
  return !proxy->RunChildApplication(argv+1);
}
