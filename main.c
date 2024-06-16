#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>

#include <SDL.h>
#include <SDL_ttf.h>
#include <png.h>
#include <gmp.h>

struct worker {
    SDL_Thread *thread;
    SDL_mutex *mutex;
    SDL_cond *cond;
    SDL_Rect work;
    struct render_ctx *ctx;
    struct worker *next_free;
};

struct render_ctx {
    SDL_cond *cond, *lcond;
    SDL_mutex *mutex;
    mpf_t re, im, scale;
    int iters, width, height;
    void *gradient;
    int gwidth;
    bool *quit, resized, needs_rerender;
    uint32_t *pixels;
    struct worker *free;
    struct worker workers[64];
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

void render_fract_rect(const struct worker *worker)
{
    const struct render_ctx *c = worker->ctx;
    const SDL_Rect work = worker->work;
    uint32_t *pixels = c->pixels;
    const int gradient_width = c->gwidth;
    const uint32_t *gradient = c->gradient;
    const int w = c->width, h = c->height;
    const int iters = c->iters;
    const mpf_t *pr = &c->re;
    const mpf_t *pi = &c->im;
    const mpf_t *scale = &c->scale;

    int y;
    for (y = work.y; y < work.h + work.y; ++y) {
        int x;
        mpf_t re, im;

        mpf_inits(re, im, NULL);
        for (x = work.x; x < work.w + work.x; ++x) {
            double v;
            mpf_set_si(re, x-w/2);
            mpf_div(re, re, *scale);
            mpf_add(re, re, *pr);
            mpf_set_si(im, y-h/2);
            mpf_div(im, im, *scale);
            mpf_add(im, im, *pi);
            v = fract_dot(iters, re, im);
            pixels[y*w + x] = gradient[(int)(v*gradient_width)]|0xff000000;
        }
        mpf_clears(re, im, NULL);
    }
}

int worker_thread(void *data)
{
    struct worker *w = data;

    SDL_LockMutex(w->mutex);
    for (;!*w->ctx->quit;) {
        SDL_LockMutex(w->ctx->mutex);
        w->next_free = w->ctx->free;
        w->ctx->free = w;
        SDL_UnlockMutex(w->ctx->mutex);
        SDL_CondSignal(w->ctx->lcond);
        SDL_CondWait(w->cond, w->mutex);
        render_fract_rect(w);
    }
    SDL_LockMutex(w->ctx->mutex);
    w->next_free = w->ctx->free;
    w->ctx->free = w;
    SDL_UnlockMutex(w->ctx->mutex);
    SDL_CondSignal(w->ctx->lcond);
    SDL_UnlockMutex(w->mutex);

    return 0;
}

#define ARRAY_LEN(x) (sizeof(x)/sizeof(*(x)))

size_t free_depth(struct worker *free)
{
    size_t i;
    for (i = 0; free; free = free->next_free)
        i++;
    return i;
}

void park_all_workers(struct render_ctx *ctx)
{
    size_t i, j;
    for (i = 0, j = 0; i < ARRAY_LEN(ctx->workers); ++i) {
        if (!ctx->workers[i].work.w) continue;
        ctx->workers[i].work.w = 0;
        j++;
    }

    for (i = 0; i < j && free_depth(ctx->free) < j; ++i) {
        SDL_CondWait(ctx->lcond, ctx->mutex);
    }
}

void workgiving(struct render_ctx *ctx)
{
    const int w = ctx->width, h = ctx->height;
    int y = 0;
    while (y < h) {
        int work_h = 64, x = 0;
        if (work_h > h - y) work_h = h - y;
        while (x < w) {
            int work_w = 64;
            struct worker *old_free;
            if (work_w > w - x) work_w = w - x;
            if (!ctx->free) {
                SDL_CondWait(ctx->lcond, ctx->mutex);
            }
            if (!ctx->resized || *ctx->quit) {
                park_all_workers(ctx);
                return;
            }

            old_free = ctx->free;
            SDL_LockMutex(old_free->mutex);
            old_free->work.x = x;
            old_free->work.y = y;
            old_free->work.w = work_w;
            old_free->work.h = work_h;
            ctx->free = old_free->next_free;
            old_free->next_free = NULL;
            SDL_UnlockMutex(old_free->mutex);
            SDL_CondSignal(old_free->cond);

            x += work_w;
        }
        y += work_h;
    }
}

int render_thread(void *data)
{
    struct render_ctx *ctx = data;
    int i, workers_avail = SDL_GetCPUCount();
    /* For HI-end cpus with 64+ threads */
    if (workers_avail > ARRAY_LEN(ctx->workers)) workers_avail = ARRAY_LEN(ctx->workers);

    SDL_LockMutex(ctx->mutex);
    for (i = 0; i < workers_avail; ++i) {
        char buf[256];
        struct worker *w = &ctx->workers[i];
        w->ctx = ctx;
        w->mutex = SDL_CreateMutex();
        w->cond = SDL_CreateCond();
        sprintf(buf, "Worker%d", i);
        w->thread = SDL_CreateThread(worker_thread, buf, w);
    }

    while (!*ctx->quit) {
        if (!ctx->resized) {
            ctx->pixels = realloc(ctx->pixels, sizeof(*ctx->pixels) * ctx->width * ctx->height);
            ctx->resized = true;
        }

        ctx->needs_rerender = false;
        workgiving(ctx);
        if (!ctx->resized) continue;
        if (!*ctx->quit && !ctx->needs_rerender) SDL_CondWait(ctx->cond, ctx->mutex);
    }
    SDL_UnlockMutex(ctx->mutex);
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

void render_coords(struct render_ctx *ctx, SDL_Renderer *renderer, TTF_Font* font)
{
    char buf[256];
    SDL_Texture *texture;
    SDL_Surface *surface;
    SDL_Color color = {255, 255, 255, 255};
    SDL_Rect dest;

    gmp_snprintf(buf, sizeof(buf), "re: %.32Ff\nim: %.32Ff\nscale: %Ff\niters: %d",
            ctx->re, ctx->im, ctx->scale, ctx->iters);
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
    int i;
    float scd = 1;
    SDL_Renderer *renderer;
    SDL_Window *window;
    SDL_Texture *texture;
    TTF_Font *font;
    SDL_Thread *thread;
    bool quit;
    struct render_ctx *rctx;

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
    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, width, height);
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);

    rctx = malloc(sizeof(*rctx));
    memset(rctx, 0, sizeof(*rctx));
    rctx->mutex = SDL_CreateMutex();
    rctx->cond = SDL_CreateCond();
    rctx->lcond = SDL_CreateCond();

    quit = false;

    rctx->quit = &quit;
    rctx->gradient = gradient_pixels;
    rctx->gwidth = gradient_width;

    mpf_inits(rctx->im, rctx->re, NULL);
    mpf_init_set_si(rctx->scale, 100);
    rctx->iters = 10;

    rctx->width = width;
    rctx->height = height;

    thread = SDL_CreateThread(render_thread, "RenderThread", rctx);
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
                    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, width*scd, height*scd);
                    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
                    TTF_SetFontSize(font, (float)height/30);
                    rctx->width = width*scd;
                    rctx->height = height*scd;
                    rctx->resized = false;
                    rctx->needs_rerender = true;

                    SDL_CondSignal(rctx->cond);
                }
            } break;
            case SDL_KEYDOWN: {
                mpf_t t;
                mpf_init_set_ui(t, 100);
                mpf_div(t, t, rctx->scale);
                switch (event.key.keysym.sym) {
                case SDLK_w: mpf_sub(rctx->im, rctx->im, t); break;
                case SDLK_s: mpf_add(rctx->im, rctx->im, t); break;
                case SDLK_a: mpf_sub(rctx->re, rctx->re, t); break;
                case SDLK_d: mpf_add(rctx->re, rctx->re, t); break;
                case SDLK_x: rctx->iters -= 10; break;
                case SDLK_c: rctx->iters += 10; break;
                case SDLK_t: if (scd > 0.1f) scd -= 0.1f; goto resize;
                case SDLK_y: if (scd < 1.0f) scd += 0.1f; goto resize;
                case SDLK_r: scd = 1.0f; goto resize;

                case SDLK_p:
                    precision += 10;
                    mpf_set_default_prec(precision);
                    reinit_pos(rctx);
                    break;
                case SDLK_o:
                    precision -= 10;
                    mpf_set_default_prec(precision);
                    reinit_pos(rctx);
                    break;

                case SDLK_SPACE: mpf_mul_ui(rctx->scale, rctx->scale, 2); break;
                case SDLK_u: mpf_div_ui(rctx->scale, rctx->scale, 2); break;
                default: goto notrig;
                }
                rctx->needs_rerender = true;
                SDL_CondSignal(rctx->cond);
                notrig:
                mpf_clear(t);
            } break;
            case SDL_MOUSEBUTTONDOWN: {
                int x, y;
                mpf_t re, im;
                x = event.button.x*scd;
                y = event.button.y*scd;
                mpf_init_set_si(re, x-width*scd/2);
                mpf_div(re, re, rctx->scale);
                mpf_add(re, re, rctx->re);
                mpf_init_set_si(im, y-height*scd/2);
                mpf_div(im, im, rctx->scale);
                mpf_add(im, im, rctx->im);
                mpf_set(rctx->im, im);
                mpf_set(rctx->re, re);
                mpf_clears(re, im, NULL);
                rctx->needs_rerender = true;
                SDL_CondSignal(rctx->cond);
            } break;
            }
        }

        SDL_SetRenderDrawColor(renderer, 0x18, 0x18, 0x18, 0xff);
        SDL_RenderClear(renderer);

        if (rctx->resized) {
            SDL_UpdateTexture(texture, NULL, rctx->pixels, rctx->width*sizeof(uint32_t));
        }

        SDL_RenderCopy(renderer, texture, NULL, NULL);
        SDL_SetRenderDrawColor(renderer, 0xff, 0x00, 0x00, 0xff);
        for (i = 0; i < SDL_GetCPUCount(); ++i) {
            SDL_Rect work = rctx->workers[i].work;
            work.x = (float)work.x/scd;
            work.y = (float)work.y/scd;
            work.w = (float)work.w/scd;
            work.h = (float)work.h/scd;
            SDL_RenderDrawRect(renderer, &work);
        }
        render_coords(rctx, renderer, font);

        SDL_RenderPresent(renderer);
    }
    SDL_CondSignal(rctx->cond);
    SDL_CondSignal(rctx->lcond);
    SDL_WaitThread(thread, NULL);

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    free(gradient_pixels);
    free(rctx->pixels);
    SDL_DestroyMutex(rctx->mutex);
    SDL_DestroyCond(rctx->cond);
    mpf_clears(rctx->im, rctx->re, NULL);
    free(rctx);
    TTF_CloseFont(font);

    return 0;
}
