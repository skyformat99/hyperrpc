package(default_visibility = ["//visibility:public"])

load("hrpc", "cc_hrpc_library")

cc_library(
  name = "hyperrpc",
  includes = ["src"],
  copts = [
    "-g",
    "-O2",
    "-Wall",
  ],
  linkopts = [
    "-lrt",
  ],
  nocopts = "-fPIC",
  linkstatic = 1,
  srcs = glob([
    "src/hyperrpc/*.cc",
    "src/hyperrpc/*.h",
  ]),
  deps = [
    "//hyperudp",
    "//third_party/protobuf",
  ],
)

cc_binary(
  name = "protoc-gen-hrpc_cpp",
  copts = [
    "-g",
    "-O0",
    "-Wall",
  ],
  nocopts = "-fPIC",
  linkstatic = 1,
  srcs = glob([
    "src/hyperrpc/compiler/*.cc",
    "src/hyperrpc/compiler/*.h",
  ]),
  deps = [
    ":hyperrpc",
    "//third_party/protobuf:protoc_lib",
  ],
  malloc = "//third_party/jemalloc-360"
)

cc_test(
  name = "test",
  copts = [
    "-g",
    "-O2",
    "-Wall",
    "-fno-strict-aliasing",
  ],
  nocopts = "-fPIC",
  linkstatic = 1,
  srcs = glob(["test/*_test.cc"]),
  deps = [
    ":hyperrpc",
    "//gtestx",
  ],
  malloc = "//third_party/jemalloc-360"
)

cc_hrpc_library(
  name = "rpc_example_proto",
  srcs = glob(["example/*.proto"]),
  deps = [],
)

cc_binary(
  name = "rpc_server",
  copts = [
    "-g",
    "-O2",
    "-Wall",
  ],
  nocopts = "-fPIC",
  linkstatic = 1,
  srcs = ["example/rpc_server.cc"],
  deps = [
    ":hyperrpc",
  ],
  malloc = "//third_party/jemalloc-360"
)

cc_binary(
  name = "rpc_client",
  copts = [
    "-g",
    "-O2",
    "-Wall",
  ],
  nocopts = "-fPIC",
  linkstatic = 1,
  srcs = ["example/rpc_client.cc"],
  deps = [
    ":hyperrpc",
  ],
  malloc = "//third_party/jemalloc-360"
)

