#ifndef AIOS_APPS_AI_CHAT_H
#define AIOS_APPS_AI_CHAT_H

/* kernel/apps/ai_chat.h — Phase 11.5
 *
 * GUI AI Chat application — thin windowed frontend for the
 * kernel/llm inference manager.
 *
 * Features (initial):
 *   • Text log area showing conversation history
 *   • Single-line input box at bottom
 *   • Press Enter to send user message and stream LLM reply
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef struct ai_chat_s {
    uint32_t win_id;         /* WM window ID           */
    char    *log_buf;        /* chat log text buffer   */
    size_t   log_size;       /* allocated bytes        */
    size_t   log_len;        /* used bytes             */

    char    *input_buf;      /* current input line     */
    size_t   input_size;     /* allocated bytes        */
    size_t   input_len;      /* used bytes             */
} ai_chat_t;

ai_chat_t *ai_chat_open(void);
void       ai_chat_close(ai_chat_t *chat);

#endif /* AIOS_APPS_AI_CHAT_H */
