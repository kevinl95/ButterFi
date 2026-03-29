#include "butterfi_session.h"

#include <string.h>

static bool butterfi_session_chunk_is_set(const struct butterfi_session *session, uint8_t chunk_idx)
{
    uint8_t byte_idx = (uint8_t)(chunk_idx / 8u);
    uint8_t bit_mask = (uint8_t)(1u << (chunk_idx % 8u));

    return (session->received_chunks[byte_idx] & bit_mask) != 0u;
}

static void butterfi_session_chunk_set(struct butterfi_session *session, uint8_t chunk_idx)
{
    uint8_t byte_idx = (uint8_t)(chunk_idx / 8u);
    uint8_t bit_mask = (uint8_t)(1u << (chunk_idx % 8u));

    session->received_chunks[byte_idx] |= bit_mask;
}

static uint8_t butterfi_session_find_next_needed(const struct butterfi_session *session)
{
    uint16_t idx;

    if (session->total_chunks == 0u) {
        return 0u;
    }

    for (idx = 0u; idx < session->total_chunks; ++idx) {
        if (!butterfi_session_chunk_is_set(session, (uint8_t)idx)) {
            return (uint8_t)idx;
        }
    }

    return session->total_chunks;
}

void butterfi_session_init(struct butterfi_session *session, uint32_t resend_timeout_ms)
{
    if (session == NULL) {
        return;
    }

    memset(session, 0, sizeof(*session));
    session->resend_timeout_ms = resend_timeout_ms;
}

void butterfi_session_reset(struct butterfi_session *session)
{
    uint32_t resend_timeout_ms;

    if (session == NULL) {
        return;
    }

    resend_timeout_ms = session->resend_timeout_ms;
    memset(session, 0, sizeof(*session));
    session->resend_timeout_ms = resend_timeout_ms;
}

enum butterfi_session_status butterfi_session_begin_request(struct butterfi_session *session,
                                                            uint8_t request_id,
                                                            uint32_t now_ms)
{
    if (session == NULL) {
        return BUTTERFI_SESSION_INVALID_ARGUMENT;
    }

    if (session->active) {
        return BUTTERFI_SESSION_BUSY;
    }

    memset(session->received_chunks, 0, sizeof(session->received_chunks));
    session->active = true;
    session->request_id = request_id;
    session->total_chunks = 0u;
    session->next_needed_chunk = 0u;
    session->highest_chunk_seen = 0u;
    session->last_update_ms = now_ms;
    session->resend_deadline_ms = now_ms + session->resend_timeout_ms;

    return BUTTERFI_SESSION_OK;
}

enum butterfi_session_status butterfi_session_record_chunk(struct butterfi_session *session,
                                                           uint8_t request_id,
                                                           uint8_t chunk_idx,
                                                           uint8_t total_chunks,
                                                           uint32_t now_ms,
                                                           bool *is_complete,
                                                           bool *is_new_chunk)
{
    bool duplicate = false;

    if ((session == NULL) || (is_complete == NULL) || (is_new_chunk == NULL)) {
        return BUTTERFI_SESSION_INVALID_ARGUMENT;
    }

    if (!session->active) {
        return BUTTERFI_SESSION_NO_ACTIVE_REQUEST;
    }

    if (request_id != session->request_id) {
        return BUTTERFI_SESSION_WRONG_REQUEST;
    }

    if ((total_chunks == 0u) || (chunk_idx >= total_chunks) || (total_chunks > BUTTERFI_SESSION_MAX_CHUNKS)) {
        return BUTTERFI_SESSION_INVALID_CHUNK;
    }

    if ((session->total_chunks != 0u) && (session->total_chunks != total_chunks)) {
        return BUTTERFI_SESSION_INVALID_CHUNK;
    }

    session->total_chunks = total_chunks;
    duplicate = butterfi_session_chunk_is_set(session, chunk_idx);
    if (!duplicate) {
        butterfi_session_chunk_set(session, chunk_idx);
        if (chunk_idx > session->highest_chunk_seen) {
            session->highest_chunk_seen = chunk_idx;
        }
    }

    session->next_needed_chunk = butterfi_session_find_next_needed(session);
    session->last_update_ms = now_ms;
    session->resend_deadline_ms = now_ms + session->resend_timeout_ms;

    *is_complete = butterfi_session_is_complete(session);
    *is_new_chunk = !duplicate;

    if (duplicate) {
        return BUTTERFI_SESSION_DUPLICATE_CHUNK;
    }

    return BUTTERFI_SESSION_OK;
}

bool butterfi_session_should_resend(const struct butterfi_session *session, uint32_t now_ms)
{
    if ((session == NULL) || !session->active) {
        return false;
    }

    if (butterfi_session_is_complete(session)) {
        return false;
    }

    return now_ms >= session->resend_deadline_ms;
}

enum butterfi_session_status butterfi_session_get_next_needed_chunk(const struct butterfi_session *session,
                                                                    uint8_t *chunk_idx)
{
    if ((session == NULL) || (chunk_idx == NULL)) {
        return BUTTERFI_SESSION_INVALID_ARGUMENT;
    }

    if (!session->active) {
        return BUTTERFI_SESSION_NO_ACTIVE_REQUEST;
    }

    *chunk_idx = session->next_needed_chunk;
    return BUTTERFI_SESSION_OK;
}

enum butterfi_session_status butterfi_session_request_resend_from(struct butterfi_session *session,
                                                                  uint8_t chunk_idx,
                                                                  uint32_t now_ms)
{
    if (session == NULL) {
        return BUTTERFI_SESSION_INVALID_ARGUMENT;
    }

    if (!session->active) {
        return BUTTERFI_SESSION_NO_ACTIVE_REQUEST;
    }

    if ((session->total_chunks != 0u) && (chunk_idx >= session->total_chunks)) {
        return BUTTERFI_SESSION_INVALID_CHUNK;
    }

    session->next_needed_chunk = chunk_idx;
    session->last_update_ms = now_ms;
    session->resend_deadline_ms = now_ms;

    return BUTTERFI_SESSION_OK;
}

void butterfi_session_note_resend(struct butterfi_session *session, uint32_t now_ms)
{
    if ((session == NULL) || !session->active) {
        return;
    }

    session->last_update_ms = now_ms;
    session->resend_deadline_ms = now_ms + session->resend_timeout_ms;
}

bool butterfi_session_is_complete(const struct butterfi_session *session)
{
    if ((session == NULL) || !session->active || (session->total_chunks == 0u)) {
        return false;
    }

    return session->next_needed_chunk >= session->total_chunks;
}