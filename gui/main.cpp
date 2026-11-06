#include "include/core/SkCanvas.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkSurface.h"

#include <cstdio>

int main() {
  sk_sp<SkSurface> surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(8, 8));
  if (!surface)
    return 1;
  surface->getCanvas()->clear(SK_ColorBLACK);
  std::puts("skia ok");
  return 0;
}
