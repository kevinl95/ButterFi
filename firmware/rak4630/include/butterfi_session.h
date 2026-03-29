#ifndef BUTTERFI_SESSION_H
#define BUTTERFI_SESSION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BUTTERFI_SESSION_MAX_CHUNKS 255u
#define BUTTERFI_SESSION_CHUNK_BITMAP_BYTES 32u

enum butterfi_session_status {
    BUTTERFI_SESSION_OK = 0,
    BUTTERFI_SESSION_BUSY,
    BUTTERFI_SESSION_NO_ACTIVE_REQUEST,
    BUTTERFI_SESSION_WRONG_REQUEST,
    BUTTERFI_SESSION_INVALID_CHUNK,
    BUTTERFI_SESSION_DUPLICATE_CHUNK,
    BUTTERFI_SESSION_INVALID_ARGUMENT,
};

struct butterfi_session {
    bool active;
    uint8_t request_id;
    uint8_t total_chunks;
    uint8_t next_needed_chunk;
    uint8_t highest_chunk_seen;
    uint32_t last_update_ms;
    uint32_t resend_deadline_ms;
    uint32_t resend_timeout_ms;
    uint8_t received_chunks[BUTTERFI_SESSION_CHUNK_BITMAP_BYTES];
};

void butterfi_session_init(struct butterfi_session *session, uint32_t resend_timeout_ms);
void butterfi_session_reset(struct butterfi_session *session);

enum butterfi_session_status butterfi_session_begin_request(struct butterfi_session *session,
                                                            uint8_t request_id,
                                                            uint32_t now_ms);

enum butterfi_session_status butterfi_session_record_chunk(struct butterfi_session *session,
                                                           uint8_t request_id,
                                                           uint8_t chunk_idx,
                                                           uint8_t total_chunks,
                                                           uint32_t now_ms,
                                                           bool *is_complete,
                                                           bool *is_new_chunk);

bool butterfi_session_should_resend(const struct butterfi_session *session, uint32_t now_ms);

enum butterfi_session_status butterfi_session_get_next_needed_chunk(const struct butterfi_session *session,
                                                                    uint8_t *chunk_idx);

enum butterfi_session_status butterfi_session_request_resend_from(struct butterfi_session *session,
                                                                  uint8_t chunk_idx,
                                                                  uint32_t now_ms);

void butterfi_session_note_resend(struct butterfi_session *session, uint32_t now_ms);
bool butterfi_session_is_complete(const struct butterfi_session *session);

#endif