#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>

#include <SDL.h>
#include <SDL_ttf.h>
#include <png.h>
#include <gmp.h>

struct render_ctx {
    SDL_cond *cond;
    SDL_mutex *mutex;
    mpf_t re, im, scale;
    int iters, width, height;
    void *gradient;
    int gwidth;
    bool *quit, resized, needs_rerender;
    uint32_t *pixels;
};

uint32_t *load_image(const char *path, int *w, int *h)
{
    uint32_t *pixels;
    png_image image = {0};
    image.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_file(&image, path)) {
        fprintf(stderr, "ERROR: could not load image %s: %s\n",
                path, image.message);
        return NULL;
    }
    *w = image.width;
    *h = image.height;
    pixels = malloc(sizeof(*pixels) * image.width * image.height);
    image.format = PNG_FORMAT_ARGB;
    if (!png_image_finish_read(&image, NULL, pixels, 0, NULL)) {
        free(pixels);
        fprintf(stderr, "ERROR: could not load image %s: %s\n",
                path, image.message);
        return NULL;
    }
    return pixels;
}

bool is_dot_out(mpf_t zr, mpf_t zi)
{
    bool result;
    mpf_t a, b;
    mpf_inits(a, b, NULL);
    mpf_pow_ui(a, zr, 2);
    mpf_pow_ui(b, zi, 2);
    mpf_add(a, a, b);
    result = mpf_cmp_si(a, 4) > 0;
    mpf_clears(a, b, NULL);
    return result;
}

double fract_dot(int iters, mpf_t re, mpf_t im)
{
    mpf_t zre, zim;
    mpf_t t;
    mpf_t u;
    int i;
    mpf_inits(u, t, NULL);
    mpf_init_set(zre, re);
    mpf_init_set(zim, im);
    for (i = 0; i < iters && !is_dot_out(zre, zim); ++i) {
        mpf_set(t, zre);
        mpf_pow_ui(u, zim, 2);
        mpf_pow_ui(zre, zre, 2);
        mpf_sub(zre, zre, u);
        mpf_add(zre, zre, re);

        mpf_mul_ui(zim, zim, 2);
        mpf_mul(zim, zim, t);
        mpf_add(zim, zim, im);
    }

    mpf_clears(zre, zim, t, u, NULL);

    return (double)i/iters;
}

void render_fract(mpf_t pr, mpf_t pi,
                  mpf_t scale, uint32_t *gradient,
                  int gradient_width,
                  int iters, int w, int h,
                  uint32_t *pixels)
{
    int y;
    #pragma omp parallel for
    for (y = 0; y < h; ++y) {
        int x;
        mpf_t re, im;

        mpf_inits(re, im, NULL);
        for (x = 0; x < w; ++x) {
            double v;
            mpf_set_si(re, x-w/2);
            mpf_div(re, re, scale);
            mpf_add(re, re, pr);
            mpf_set_si(im, y-h/2);
            mpf_div(im, im, scale);
            mpf_add(im, im, pi);
            v = fract_dot(iters, re, im);
            pixels[y*w + x] = gradient[(int)(v*gradient_width)]|0xff000000;
        }
        mpf_clears(re, im, NULL);
    }
}

int render_thread(void *data)
{
    struct render_ctx *ctx = data;

    while (!*ctx->quit) {
        if (!ctx->resized) {
            ctx->pixels = realloc(ctx->pixels, sizeof(*ctx->pixels) * ctx->width * ctx->height);
            ctx->resized = true;
        }

        ctx->needs_rerender = false;
        render_fract(ctx->re, ctx->im, ctx->scale, ctx->gradient, ctx->gwidth, ctx->iters,
                     ctx->width, ctx->height, ctx->pixels);
        SDL_LockMutex(ctx->mutex);
        if (!*ctx->quit && !ctx->needs_rerender) SDL_CondWait(ctx->cond, ctx->mutex);
        SDL_UnlockMutex(ctx->mutex);
    }
    return 0;
}

void reinit_pos(struct render_ctx *ctx)
{
    mpf_t nre, nim;
    mpf_init_set(nre, ctx->re);
    mpf_init_set(nim, ctx->im);
    mpf_clears(ctx->re, ctx->im, NULL);
    memcpy(ctx->re, nre, sizeof(nre));
    memcpy(ctx->im, nim, sizeof(nim));
}

void render_coords(struct render_ctx ctx, SDL_Renderer *renderer, TTF_Font* font)
{
    char buf[256];
    SDL_Texture *texture;
    SDL_Surface *surface;
    SDL_Color color = {255, 255, 255, 255};
    SDL_Rect dest;

    gmp_snprintf(buf, sizeof(buf), "re: %.32Ff\nim: %.32Ff\nscale: %Ff\niters: %d", ctx.re, ctx.im, ctx.scale, ctx.iters);
    surface = TTF_RenderUTF8_Blended_Wrapped(font, buf, color, 0);
    texture = SDL_CreateTextureFromSurface(renderer, surface);
    dest.x = 20;
    dest.y = 20;
    dest.w = surface->w;
    dest.h = surface->h;
    SDL_FreeSurface(surface);
    SDL_RenderCopy(renderer, texture, NULL, &dest);
    SDL_DestroyTexture(texture);
}

int main(int argc, char **argv)
{
    const char *gradient_path;
    uint32_t *gradient_pixels;
    int gradient_width, gradient_height, width, height, precision = 16;
    float scd = 1;
    SDL_Renderer *renderer;
    SDL_Window *window;
    SDL_Texture *texture;
    TTF_Font *font;
    SDL_Thread *thread;
    bool quit;
    struct render_ctx render_ctx = {0};

    mpf_set_default_prec(precision);
    
    if (argc < 2) {
        fprintf(stderr, "ERROR: gradient file expected\n");
        exit(1);
    }

    gradient_path = argv[1];
    gradient_pixels = load_image(gradient_path, &gradient_width, &gradient_height);
    if (gradient_pixels == NULL) {
        exit(1);
    }

    if (TTF_Init() < 0) {
        fprintf(stderr, "ERROR: could not init TTF: %s\n", TTF_GetError());
        exit(1);
    }

    font = TTF_OpenFont("font.ttf", 32);
    if (font == NULL) {
        fprintf(stderr, "ERROR: could not open font: %s\n", TTF_GetError());
        exit(1);
    }

    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "ERROR: could not init SDL: %s\n",
                SDL_GetError());
        exit(1);
    }

    window = SDL_CreateWindow("Fract", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 800, 600, SDL_WINDOW_RESIZABLE);
    if (window == NULL) {
        fprintf(stderr, "ERROR: could not create window: %s\n",
                SDL_GetError());
        exit(1);
    }

    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_PRESENTVSYNC | SDL_RENDERER_ACCELERATED);
    if (renderer == NULL) {
        fprintf(stderr, "ERROR: could not create renderer: %s\n",
                SDL_GetError());
        exit(1);
    }

    SDL_GetWindowSize(window, &width, &height);
    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, width, height);
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    
    render_ctx.mutex = SDL_CreateMutex();
    render_ctx.cond = SDL_CreateCond();

    quit = false;

    render_ctx.quit = &quit;
    render_ctx.gradient = gradient_pixels;
    render_ctx.gwidth = gradient_width;

    mpf_inits(render_ctx.im, render_ctx.re, NULL);
    mpf_init_set_si(render_ctx.scale, 100);
    render_ctx.iters = 10;

    render_ctx.width = width;
    render_ctx.height = height;

    thread = SDL_CreateThread(render_thread, "RenderThread", &render_ctx);
    if (thread == NULL) {
        fprintf(stderr, "ERROR: could not create thread: %s\n",
                SDL_GetError());
        exit(1);
    }

    while (!quit) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
            case SDL_QUIT: quit = true; break;
            case SDL_WINDOWEVENT: {
                if (event.window.event == SDL_WINDOWEVENT_RESIZED) {
                    SDL_GetWindowSize(window, &width, &height);
resize:
                    SDL_DestroyTexture(texture);
                    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, width*scd, height*scd);
                    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
                    TTF_SetFontSize(font, (float)height/30);
                    render_ctx.width = width*scd;
                    render_ctx.height = height*scd;
                    render_ctx.resized = false;
                    render_ctx.needs_rerender = true;

                    SDL_CondSignal(render_ctx.cond);
                }
            } break;
            case SDL_KEYDOWN: {
                mpf_t t;
                mpf_init_set_ui(t, 100);
                mpf_div(t, t, render_ctx.scale);
                switch (event.key.keysym.sym) {
                case SDLK_w: mpf_sub(render_ctx.im, render_ctx.im, t); break;
                case SDLK_s: mpf_add(render_ctx.im, render_ctx.im, t); break;
                case SDLK_a: mpf_sub(render_ctx.re, render_ctx.re, t); break;
                case SDLK_d: mpf_add(render_ctx.re, render_ctx.re, t); break;
                case SDLK_x: render_ctx.iters -= 10; break;
                case SDLK_c: render_ctx.iters += 10; break;
                case SDLK_t: if (scd > 0.1f) scd -= 0.1f; goto resize;
                case SDLK_y: if (scd < 1.0f) scd += 0.1f; goto resize;
                case SDLK_r: scd = 1.0f; goto resize;

                case SDLK_p:
                    precision += 10;
                    mpf_set_default_prec(precision);
                    reinit_pos(&render_ctx);
                    break;
                case SDLK_o:
                    precision -= 10;
                    mpf_set_default_prec(precision);
                    reinit_pos(&render_ctx);
                    break;

                case SDLK_SPACE: mpf_mul_ui(render_ctx.scale, render_ctx.scale, 2); break;
                case SDLK_u: mpf_div_ui(render_ctx.scale, render_ctx.scale, 2); break;
                default: goto notrig;
                }
                render_ctx.needs_rerender = true;
                SDL_CondSignal(render_ctx.cond);
                notrig:
                mpf_clear(t);
            } break;
            case SDL_MOUSEBUTTONDOWN: {
                int x, y;
                mpf_t re, im;
                x = event.button.x*scd;
                y = event.button.y*scd;
                mpf_init_set_si(re, x-width*scd/2);
                mpf_div(re, re, render_ctx.scale);
                mpf_add(re, re, render_ctx.re);
                mpf_init_set_si(im, y-height*scd/2);
                mpf_div(im, im, render_ctx.scale);
                mpf_add(im, im, render_ctx.im);
                mpf_set(render_ctx.im, im);
                mpf_set(render_ctx.re, re);
                mpf_clears(re, im, NULL);
                render_ctx.needs_rerender = true;
                SDL_CondSignal(render_ctx.cond);
            } break;
            }
        }

        SDL_SetRenderDrawColor(renderer, 0x18, 0x18, 0x18, 0xff);
        SDL_RenderClear(renderer);
        if (render_ctx.resized) {
            SDL_UpdateTexture(texture, NULL, render_ctx.pixels, render_ctx.width*sizeof(uint32_t));
        }
        SDL_RenderCopy(renderer, texture, NULL, NULL);
        render_coords(render_ctx, renderer, font);

        SDL_RenderPresent(renderer);
    }
    SDL_CondSignal(render_ctx.cond);
    SDL_WaitThread(thread, NULL);

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    free(gradient_pixels);
    free(render_ctx.pixels);
    SDL_DestroyMutex(render_ctx.mutex);
    SDL_DestroyCond(render_ctx.cond);
    mpf_clears(render_ctx.im, render_ctx.re, NULL);
    TTF_CloseFont(font);
    
    return 0;
}
