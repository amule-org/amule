//
// This file is part of the aMule Project.
//
// Copyright (c) 2026 aMule Team ( https://amule-org.github.io )
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA.
//

(() => {
  "use strict";

  const selector = "a[href], area[href]";

  function repair(element) {
    if (!(element instanceof Element) || !element.matches(selector)) return;
    const href = element.getAttribute("href");
    // Leave filename escapes, other protocols and already encoded links intact.
    if (href && /^\s*ed2k:\/\//i.test(href) && href.includes("|")) {
      // Existing %7C filename escapes become indistinguishable from newly
      // encoded separators. aMule can then reject the link or read filename
      // text as its size/hash. Keep the original for copy/paste instead.
      const fileName = href.match(/^\s*ed2k:\/\/\|file\|([^|]*)\|/i)?.[1];
      if (fileName && /%7c/i.test(fileName)) return;
      element.setAttribute("href", href.replace(/\|/g, "%7C"));
    }
  }

  function scan(root) {
    repair(root);
    root.querySelectorAll?.(selector).forEach(repair);
  }

  // Repair before activation without replacing the click or losing its user gesture.
  // composedPath also finds anchors inside an open shadow root.
  function beforeActivation(event) {
    for (const element of event.composedPath()) {
      if (element instanceof Element && element.matches(selector)) {
        // A site's click handler may replace a shadow link's href after this
        // capture listener. Observe that root before the handler runs so the
        // replacement is repaired before the browser's default activation.
        const root = element.getRootNode();
        if (root instanceof ShadowRoot) observe(root);
        repair(element);
        break;
      }
    }
  }

  function installActivationListeners() {
    for (const event of [
      "pointerdown",
      "click",
      "auxclick",
      "contextmenu",
      "keydown",
    ]) {
      document.addEventListener(event, beforeActivation, {
        capture: true,
        passive: true,
      });
    }
  }
  installActivationListeners();

  // Initial links and later page updates also need fixing before context menus
  // and keyboard navigation. This does not add listeners to individual links.
  const observer = new MutationObserver((records) => {
    // document.open() removes document listeners but leaves this observer.
    // Reinstall when the document's children change; addEventListener ignores
    // duplicate registrations with the same callback and capture setting.
    if (
      records.some(
        (record) => record.type === "childList" && record.target === document,
      )
    ) {
      installActivationListeners();
    }
    for (const record of records) {
      if (record.type === "attributes") {
        repair(record.target);
      } else {
        record.addedNodes.forEach(scan);
      }
    }
  });
  const observedRoots = new WeakSet();
  function observe(root) {
    if (observedRoots.has(root)) return;
    observedRoots.add(root);
    observer.observe(root, {
      subtree: true,
      childList: true,
      attributes: true,
      attributeFilter: ["href"],
    });
  }
  observe(document);
  scan(document);
})();
