#include "d3d_method.h"
#include <cstdio>
#include <thread>

namespace {
using Method = int (*)(void *, int);
proxy::d3d::ResourceMethod<Method> methods;
struct Object {
  void **table;
};
int managed(void *, int value) { return value + 10; }
int dynamic(void *, int value) { return value + 20; }
int intercepted(void *object, int value) { return methods.original(object)(object, value) * 2; }
int invoke(Object &object) { return reinterpret_cast<Method>(object.table[0])(&object, 1); }
} // namespace

int main() {
  void *managedTable[] = {reinterpret_cast<void *>(&managed)};
  void *dynamicTable[] = {reinterpret_cast<void *>(&dynamic)};
  Object a{managedTable}, b{dynamicTable};
  if (!methods.install(a.table, 0, &intercepted) || !methods.install(b.table, 0, &intercepted))
    return 1;
  // Repeated creations sharing either table must not save our own hook as the
  // original, including simultaneous resource creation from different threads.
  auto repeat = [&] {
    for (unsigned i = 0; i < 1000; ++i) {
      methods.install(a.table, 0, &intercepted);
      methods.install(b.table, 0, &intercepted);
    }
  };
  std::thread first(repeat), second(repeat);
  first.join();
  second.join();
  if (invoke(a) != 22 || invoke(b) != 42) {
    std::fprintf(stderr, "Resource methods must preserve each vtable's implementation\n");
    return 1;
  }
  std::puts("Independent resource vtables and repeated installation passed");
  return 0;
}
