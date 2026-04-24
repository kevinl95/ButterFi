/*
 * butterfi_content.h
 *
 * Content routing module.
 *
 * Receives Sidewalk messages and responds with educational content
 * appropriate for the configured content package.
 *
 * In v1 this is intentionally minimal — the "content" is metadata
 * (URLs, resource lists) that the student's PWA resolves from the
 * cached bundle. The dongle doesn't serve files itself; it just
 * provides authenticated Sidewalk connectivity and config.
 */

#ifndef BUTTERFI_CONTENT_H
#define BUTTERFI_CONTENT_H

#include <sid_api.h>

/**
 * Called by main when a Sidewalk message arrives.
 * Parses the request type and queues a response via sid_put_msg.
 */
void butterfi_content_handle_msg(const struct sid_msg_desc *desc,
                                  const struct sid_msg *msg);

#endif /* BUTTERFI_CONTENT_H */
