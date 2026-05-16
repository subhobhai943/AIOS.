/*
 * shell/ai.c — shell bindings for the LLM inference manager
 *
 * Provides `ai` and `chat` commands that call into
 * kernel/llm/inference.c.
 */

#include "ai.h"

#include "../llm/inference.h"
#include "../serial.h"

static void ai_shell_token_cb(const char *text, size_t len, void *user)
{
    (void)user;
    for (size_t i = 0; i < len; i++) {
        serial_putc(text[i]);
    }
}

int ai_shell_cmd_ai(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    const char *prompt = "Hello from AIOS LLM!";
    inference_generate(prompt, 22, ai_shell_token_cb, 0);
    serial_putc('\n');
    return 0;
}

int ai_shell_cmd_chat(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    /* For now just reuse ai_shell_cmd_ai; future work: interactive chat. */
    return ai_shell_cmd_ai(0, 0);
}
