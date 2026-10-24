/* Regression for input-worker creation failure, retry, and shutdown.
 * cc -DMELEE_PC=1 -DTARGET_PC=1 -Isrc -Isrc/sdk_include \
 *    tools/test_input_poll.c $(pkg-config --cflags --libs sdl3) -o /tmp/test_input_poll
 */
#include <SDL3/SDL.h>
#include <assert.h>

static bool fail_create = true;
static int creates, joins, thread_token;
static SDL_Thread* fake_create(SDL_ThreadFunction fn, const char* name, void* data) {
    (void)fn;
    (void)name;
    (void)data;
    ++creates;
    return fail_create ? NULL : (SDL_Thread*)&thread_token;
}
static void fake_wait(SDL_Thread* thread, int* status) {
    assert(thread == (SDL_Thread*)&thread_token);
    (void)status;
    ++joins;
}
#undef SDL_CreateThread
#define SDL_CreateThread fake_create
#define SDL_WaitThread fake_wait
#include "../src/pc/input_poll.c"

void pc_log_line(const char* fmt, ...) {
    (void)fmt;
}
void pc_gcadapter_poll(void) {}
uint64_t pc_gcadapter_report_count(void) {
    return 0;
}
bool pc_gcadapter_raw(int port, uint8_t raw[6], bool* wireless) {
    (void)port;
    (void)raw;
    (void)wireless;
    return false;
}
bool pc_gcadapter_status(int port, struct PADStatus* out) {
    (void)port;
    (void)out;
    return false;
}

int main(void) {
    assert(!pc_input_poll_is_running());
    pc_input_poll_init();
    assert(creates == 1);
    assert(!pc_input_poll_is_running());
    pc_input_poll_shutdown();
    assert(joins == 0);

    fail_create = false;
    pc_input_poll_init();
    assert(creates == 2);
    assert(pc_input_poll_is_running());
    pc_input_poll_init();
    assert(creates == 2);
    pc_input_poll_shutdown();
    assert(joins == 1);
    assert(!pc_input_poll_is_running());
    pc_input_poll_shutdown();
    assert(joins == 1);
    return 0;
}
