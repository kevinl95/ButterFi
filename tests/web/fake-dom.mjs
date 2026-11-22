// Just enough of the DOM for butterfi-markup.js: elements, fragments, text
// nodes, append, textContent, href, dataset, classList, childElementCount.

class FakeNode {
    constructor() {
        this.childNodes = [];
    }

    append(...nodes) {
        for (const node of nodes) {
            this.childNodes.push(typeof node === "string" ? new FakeText(node) : node);
        }
    }

    get children() {
        return this.childNodes.filter((n) => n instanceof FakeElement);
    }

    get childElementCount() {
        return this.children.length;
    }

    get textContent() {
        return this.childNodes.map((n) => n.textContent).join("");
    }

    set textContent(value) {
        this.childNodes = [new FakeText(String(value))];
    }
}

class FakeText {
    constructor(text) {
        this.textContent = text;
    }
}

class FakeElement extends FakeNode {
    constructor(tagName) {
        super();
        this.tagName = tagName.toUpperCase();
        this.dataset = {};
        this.href = "";
        const classes = new Set();
        this.classList = {
            add: (c) => classes.add(c),
            contains: (c) => classes.has(c),
        };
    }
}

export function installFakeDocument() {
    globalThis.document = {
        createElement: (tag) => new FakeElement(tag),
        createDocumentFragment: () => new FakeNode(),
    };
}
