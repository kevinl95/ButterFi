// Renderer for the ButterFi markup format produced by the scraper Lambda's
// html_to_butterfi() (see template.yaml). Format summary:
//
//   # / ## / ###   heading levels 1-3
//   - text         unordered list item
//   1. text        ordered list item
//   ---            horizontal rule (also used as the separator before the
//                  trailing link reference table, when links exist)
//   >N[text]       inline link reference, resolved against a trailing
//                  ">N https://..." table at the end of the document
//
// Each non-blank line in the source is already one block-level element
// (the scraper flushes exactly one output line per heading/paragraph/li),
// so rendering is a single pass over lines with no paragraph-merging needed.

const INLINE_LINK_RE = />(\d+)\[([^\]]*)\]/g;
const LINK_TABLE_LINE_RE = /^>(\d+)\s+(\S+)$/;
const HEADING_RE = /^(#{1,3})\s?(.*)$/;
const UNORDERED_RE = /^-\s(.*)$/;
const ORDERED_RE = /^\d+\.\s(.*)$/;

/**
 * Splits off the trailing ">N url" link table (if present) and returns
 * { bodyLines, linkMap }.
 */
function extractLinkTable(lines) {
    const linkMap = new Map();
    let end = lines.length;

    while (end > 0) {
        const match = LINK_TABLE_LINE_RE.exec(lines[end - 1]);
        LINK_TABLE_LINE_RE.lastIndex = 0;
        if (!match) {
            break;
        }
        linkMap.set(match[1], match[2]);
        end -= 1;
    }

    if (linkMap.size > 0 && end > 0 && lines[end - 1] === "---") {
        end -= 1;
    }

    return { bodyLines: lines.slice(0, end), linkMap };
}

/** Appends `text`, with inline >N[label] references turned into <a> nodes. */
function appendInlineContent(el, text, linkMap) {
    let lastIndex = 0;
    for (const match of text.matchAll(INLINE_LINK_RE)) {
        if (match.index > lastIndex) {
            el.append(text.slice(lastIndex, match.index));
        }

        const [, ref, label] = match;
        const url = linkMap.get(ref);
        const anchor = document.createElement("a");
        anchor.textContent = label || url || `[${ref}]`;
        if (url) {
            anchor.href = url;
            anchor.dataset.ref = ref;
        } else {
            anchor.href = "#";
            anchor.classList.add("link-unresolved");
        }
        el.append(anchor);

        lastIndex = match.index + match[0].length;
    }

    if (lastIndex < text.length) {
        el.append(text.slice(lastIndex));
    }
}

function flushList(container, listEl) {
    if (listEl && listEl.childElementCount > 0) {
        container.append(listEl);
    }
}

/**
 * Renders assembled ButterFi markup text into a DOM fragment. Returns
 * { fragment, linkCount } so callers can show "no links on this page" state.
 */
export function renderButterfiMarkup(text) {
    const fragment = document.createDocumentFragment();
    const lines = text.split("\n");
    const { bodyLines, linkMap } = extractLinkTable(lines);

    let listEl = null;
    let listType = null;

    for (const rawLine of bodyLines) {
        const line = rawLine;

        if (line === "") {
            flushList(fragment, listEl);
            listEl = null;
            listType = null;
            continue;
        }

        if (line === "---") {
            flushList(fragment, listEl);
            listEl = null;
            listType = null;
            fragment.append(document.createElement("hr"));
            continue;
        }

        const heading = HEADING_RE.exec(line);
        if (heading) {
            flushList(fragment, listEl);
            listEl = null;
            listType = null;
            const level = heading[1].length;
            const el = document.createElement(`h${level}`);
            appendInlineContent(el, heading[2], linkMap);
            fragment.append(el);
            continue;
        }

        const unordered = UNORDERED_RE.exec(line);
        const ordered = !unordered ? ORDERED_RE.exec(line) : null;
        if (unordered || ordered) {
            const type = unordered ? "ul" : "ol";
            if (listType !== type) {
                flushList(fragment, listEl);
                listEl = document.createElement(type);
                listType = type;
            }
            const li = document.createElement("li");
            appendInlineContent(li, unordered ? unordered[1] : ordered[1], linkMap);
            listEl.append(li);
            continue;
        }

        flushList(fragment, listEl);
        listEl = null;
        listType = null;
        const p = document.createElement("p");
        appendInlineContent(p, line, linkMap);
        fragment.append(p);
    }

    flushList(fragment, listEl);

    return { fragment, linkCount: linkMap.size };
}
