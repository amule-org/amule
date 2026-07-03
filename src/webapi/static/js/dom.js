// The rendering layer: binds htm to preact's hyperscript and re-exports
// everything a view needs from a single place, so each component imports
// `html`, the hooks, and `useStore` from here.
//
//   import { html, useState, useStore } from "../dom.js";
//   export function MyView() {
//     const downloads = useStore("downloads"); // re-renders on store change
//     return html`<div>${downloads.length} files</div>`;
//   }

import { h, render } from "preact";
import { useState, useEffect } from "preact/hooks";
import htm from "htm";
import { store } from "./store.js";

// html`<tag .../>` — tagged-template JSX bound to preact's createElement.
export const html = htm.bind(h);

export { h, render };
export * from "preact/hooks";

// Bridge the pub/sub store (fed by the SSE/polling data layer in events.js)
// into preact: returns the current value for `key` and re-renders the
// component whenever the store publishes a new value for it.
export function useStore(key) {
  const [value, setValue] = useState(() => store.get(key));
  // store.subscribe() fires immediately with the current value and returns
  // its own unsubscribe fn, which is exactly what useEffect wants back.
  useEffect(() => store.subscribe(key, setValue), [key]);
  return value;
}
