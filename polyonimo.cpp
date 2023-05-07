#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <thread>
#include <iostream>
using namespace std;

#include <wayland-client.h>
#include <wayland-server.h>
#include <wayland-client-protocol.h>
#include <wayland-egl.h>

#include <syscall.h>
#include <unistd.h>
#include <sys/mman.h>

#include "xdg_shell.h"
#include "xdg_shell.c"

struct Extent{
  uint32_t width, height;
};

struct Image{
  uint32_t * pixels;
  Extent extent;
};

//TODO: soa
struct Rect{
  uint32_t x,y,width,height,color;
};

void draw_frame(Image & image, Rect const * rects, uint32_t rect_count){
  for(auto i = 0; i < rect_count; ++i){
    auto const & rect = rects[i];
    if(rect.x > image.extent.width) continue;
    if(rect.y > image.extent.height) continue;

    for(auto rect_width = rect.x; rect_width < image.extent.width and rect_width < rect.width; ++rect_width){
      for(auto rect_height = rect.x; rect_height < image.extent.height and rect_height < rect.height; ++rect_height){
        image.pixels[rect_width + (rect_height * image.extent.width)] = rect.color;
      }
    }
  }
}

void get_display_surface() noexcept;
void settup_display_wayland() noexcept;

wl_display * display = nullptr;
wl_compositor * compositor = nullptr;
wl_surface *surface = nullptr;
wl_shm *shm = nullptr;
wl_shm_pool *shm_pool = nullptr;

xdg_wm_base * wm_base = nullptr;
xdg_surface * wm_surface = nullptr;
xdg_toplevel * top_level = nullptr;

static void wm_base_handle_ping(void * data, xdg_wm_base * wm_base, uint32_t serial){
  puts("from base handle ping");
  xdg_wm_base_pong(wm_base, serial);

}
static const xdg_wm_base_listener wm_base_listener = { .ping = wm_base_handle_ping };

void xdg_surface_configure_handler(void * data, xdg_surface *surface, uint32_t serial){
  puts("from surface configure handler");
  xdg_surface_ack_configure(surface, serial);
}
const xdg_surface_listener wm_surface_listener = { .configure = xdg_surface_configure_handler };


static void global_registry_handler(void * data, struct wl_registry * registry, uint32_t id, char const * interface, uint32_t version){
  printf("Got a registry event for %s id %d\n", interface, id);
  if (strcmp(interface, "wl_compositor") == 0){
    compositor = (wl_compositor *)wl_registry_bind(registry, id, &wl_compositor_interface, 1);
    puts("found compositor");
  }
  else if (strcmp(interface, "wl_shm") == 0)
    shm = (wl_shm *)wl_registry_bind(registry, id, &wl_shm_interface, 1);
  else if (strcmp(interface, "xdg_wm_base") == 0){
    wm_base = (xdg_wm_base *)wl_registry_bind(registry, id, &xdg_wm_base_interface, 1);
    xdg_wm_base_add_listener(wm_base, &wm_base_listener, nullptr);
  }
}

static void global_registry_remover(void *data, struct wl_registry *registry, uint32_t id) {
    printf("Got a registry losing event for %d\n", id);
}

static const struct wl_registry_listener registry_listener = {
    global_registry_handler,
    global_registry_remover
};

int main(){
  settup_display_wayland();
}

void settup_display_wayland() noexcept{
  display = wl_display_connect(NULL);
  if (not display) {
	  fprintf(stderr, "Can't connect to display\n");
	  exit(1);
  } printf("connected to display\n");

  auto registry = wl_display_get_registry(display);
  wl_registry_add_listener(registry, &registry_listener, NULL);
  wl_display_dispatch(display);

  if (compositor == NULL) {
	  fprintf(stderr, "Can't find compositor\n");
	  exit(1);
  } else fprintf(stderr, "Found compositor\n");

  surface = wl_compositor_create_surface(compositor);
  if (surface == NULL) {
	  fprintf(stderr, "Can't create surface\n");
	  exit(1);
  } else fprintf(stderr, "Created surface\n");
  
  if(not wm_base){
    fprintf(stderr, "No xdg_wm_base\n");
    exit(1);
  } else fprintf(stderr, "Found wm base\n");

  wm_surface = xdg_wm_base_get_xdg_surface(wm_base, surface);
  if(not wm_surface){
    puts("no xdg surface");
    exit(1);
  }

  xdg_surface_add_listener(wm_surface, &wm_surface_listener, nullptr);

  top_level = xdg_surface_get_toplevel(wm_surface);
  if(not top_level){
    puts("no top level");
    exit(1);
  }

  struct Top_Level_State{
    int width = 300, height = 600;
    bool should_close = false;
  } top_level_state;

  auto top_level_listener = xdg_toplevel_listener{
    .configure = [](void * data, xdg_toplevel * top_level, int32_t width, int32_t height, wl_array * states){
      auto state = (Top_Level_State *)data;
      if(width) state->width = width;
      if(height) state->height = height;
      printf("configure: %dx%d\n", width, height);
    },
    .close = [](void * data, xdg_toplevel * top_level){
      auto state = (Top_Level_State *)data;
      state->should_close = true;
      puts("close top level\n");
    }
  };

  xdg_toplevel_add_listener(top_level, &top_level_listener, &top_level_state);
  wl_surface_commit(surface);
  

  wl_display_dispatch(display);

  if(not shm){
    fprintf(stderr, "No shm");
    exit(1);
  }
  int width = top_level_state.width;
  int height = top_level_state.height;
  int stride = width * 4;
  int size = width * height * stride;  // bytes, explained below
  // open an anonymous file and write some zero bytes to it
  int fd = syscall(SYS_memfd_create, "buffer", 0);
  ftruncate(fd, size);
  
  // map it to the memory
  uint8_t *data = (uint8_t*)mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  auto pixels = reinterpret_cast<uint32_t *>(data);
  uint8_t r = 0xff, g = 0x99, b = 0;

  shm_pool = wl_shm_create_pool(shm, fd, size);
  auto buffer = wl_shm_pool_create_buffer(shm_pool, 0, width, height, stride, WL_SHM_FORMAT_XRGB8888);

  //wl_display_roundtrip(display);
  wl_surface_attach(surface, buffer, 0, 0);
  //wl_surface_commit(surface);
  int cycle = 1;

  //wl_display_dispatch(display);
  auto rect = Rect{.x = 20, .y = 20, .width = 100, .height = 200, .color = 0xff00ff};

  auto image = Image{
    .pixels = pixels, 
    .extent = Extent{.width = (uint32_t)width, .height = (uint32_t)height}
  };

  for(;not top_level_state.should_close;){

    for(auto i = 0; i < (width * height); ++i){
      uint32_t pixel = 0;
      pixel += (uint32_t)r << (8*2);
      pixel += (uint32_t)g << (8);
      pixel += (uint32_t)b;

      if(r == 0xff)r =0;
      if(g == 0xff)g =0;
      if(b == 0xff)b =0;

      pixels[i] = pixel;
      r++;
      g++;
      b++;
      //     if(cycle == 1) pixels[i] = 0x00ff0000;
      //else if(cycle == 2) pixels[i] = 0x0000ff00;
      //else if(cycle == 3) pixels[i] = 0x000000ff;
      //else if(cycle == 4) pixels[i] = 0x00ff00ff;
      //else if(cycle == 5) pixels[i] = 0x00ffffff;
    }
    ++cycle;
    if(cycle > 5) cycle = 1;


    draw_frame(image, &rect, 1);

    printf("cycle %d\n", cycle);

    //
    cout << "attach\n";
    wl_surface_attach(surface, buffer, 0, 0);

    cout << "commit\n";
    wl_surface_commit(surface);
    
    //cout << "flush\n";
    //wl_display_flush(display);
    cout << "damage\n";
    wl_surface_damage(surface, 0, 0, width, height);

    cout << "round trip" << endl;
    wl_display_roundtrip(display);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

}


