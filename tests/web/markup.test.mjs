import { test } from "node:test";
import assert from "node:assert/strict";
import { installFakeDocument } from "./fake-dom.mjs";

installFakeDocument();
const { renderButterfiMarkup } = await import("../../web/butterfi-markup.js");

const shape = (fragment) => fragment.children.map((el) => el.tagName);

test("headings, paragraphs, lists and rules", () => {
    const { fragment, linkCount } = renderButterfiMarkup(
        "Title\n# One\n## Two\n### Three\nplain text\n- a\n- b\n1. first\n2. second\n---\nafter"
    );
    assert.deepEqual(shape(fragment), ["P", "H1", "H2", "H3", "P", "UL", "OL", "HR", "P"]);
    assert.equal(fragment.children[5].childElementCount, 2);
    assert.equal(fragment.children[6].children[1].textContent, "second");
    assert.equal(linkCount, 0);
});

test("inline references resolve against the trailing link table", () => {
    const text = "Read >1[the story] or >2[this].\n---\n>1 https://example.com/a\n>2 https://x.org/b";
    const { fragment, linkCount } = renderButterfiMarkup(text);
    assert.equal(linkCount, 2);
    // The separator and the table are not rendered as content.
    assert.deepEqual(shape(fragment), ["P"]);
    const [a1, a2] = fragment.children[0].children;
    assert.equal(a1.href, "https://example.com/a");
    assert.equal(a1.textContent, "the story");
    assert.equal(a1.dataset.ref, "1");
    assert.equal(a2.href, "https://x.org/b");
    assert.equal(fragment.children[0].textContent, "Read the story or this.");
});

test("a reference with no table entry is marked unresolved", () => {
    const { fragment } = renderButterfiMarkup("See >7[missing]");
    const anchor = fragment.children[0].children[0];
    assert.equal(anchor.href, "#");
    assert.ok(anchor.classList.contains("link-unresolved"));
    assert.equal(anchor.dataset.ref, undefined);
});

test("a body rule is kept when there is no link table", () => {
    const { fragment } = renderButterfiMarkup("a\n---\nb");
    assert.deepEqual(shape(fragment), ["P", "HR", "P"]);
});

test("blank lines end a list", () => {
    const { fragment } = renderButterfiMarkup("- a\n\n- b");
    assert.deepEqual(shape(fragment), ["UL", "UL"]);
});

test("empty label falls back to the URL", () => {
    const { fragment } = renderButterfiMarkup(">1[]\n---\n>1 https://e.org");
    assert.equal(fragment.children[0].children[0].textContent, "https://e.org");
});
