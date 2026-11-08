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
        repair(element);
        break;
      }
    }
  }

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

  // Initial links and later page updates also need fixing before context menus
  // and keyboard navigation. This does not add listeners to individual links.
  const observer = new MutationObserver((records) => {
    for (const record of records) {
      if (record.type === "attributes") {
        repair(record.target);
      } else {
        record.addedNodes.forEach(scan);
      }
    }
  });
  observer.observe(document, {
    subtree: true,
    childList: true,
    attributes: true,
    attributeFilter: ["href"],
  });
  scan(document);
})();
