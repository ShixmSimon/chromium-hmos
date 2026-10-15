// Copyright 2026. SPDX-License-Identifier: MPL-2.0
// Lumie scheduler for Brave's procedural selector evaluator. Runs only in the
// dedicated isolated world. JSON rules are never evaluated as JavaScript.
const previous = globalThis.__lumieAdblock;
if (previous) previous.stop();
let alive = true;
let timer = 0;
let cursor = 0;
let rules = [];
let dirty = false;
const attributes = new Set();
const observer = new MutationObserver(records => {
  if (records.some(r => r.type !== 'attributes' || !attributes.has(r.attributeName))) schedule();
});
function observe() {
  if (alive && rules.length) observer.observe(document,
    {subtree:true, childList:true, characterData:true, attributes:true});
}
function schedule() {
  if (!alive) return;
  dirty = true;
  if (timer) return;
  timer = setTimeout(pump, 50);
}
function actionOn(element, rule) {
  const action = rule.action;
  if (!action || action.type === 'style') {
    if (!element.hasAttribute(rule.attribute)) element.setAttribute(rule.attribute, '');
  } else if (action.type === 'remove') {
    element.remove();
  } else if (action.type === 'remove-attr') {
    if (element.hasAttribute(action.arg)) element.removeAttribute(action.arg);
  } else if (action.type === 'remove-class' && element.classList.contains(action.arg)) {
    element.classList.remove(action.arg);
  }
}
function pump() {
  timer = 0;
  if (!alive || !rules.length || document.hidden) return;
  if (cursor === 0) dirty = false;
  const deadline = performance.now() + 4;
  // Coalesce mutation batches into one pass. Selector calls themselves cannot
  // be preempted by JS; cap rule and result counts and yield between rules.
  let iterations = 0;
  while (iterations++ < 16 && performance.now() < deadline && cursor < rules.length) {
    const rule = rules[cursor++];
    try {
      const matches = applyCompiledSelector(rule.compiled).slice(0, 20000);
      const set = new Set(matches);
      for (const old of rule.previous) if (!set.has(old)) old.removeAttribute(rule.attribute);
      for (const element of matches) actionOn(element, rule);
      rule.previous = !rule.action || rule.action.type === 'style' ? matches : [];
    } catch { /* Bad/unsupported page selectors cannot break the document. */ }
  }
  if (cursor < rules.length) { timer = setTimeout(pump, 0); return; }
  cursor = 0;
  if (dirty) schedule();
}
function onPageHide() { clearTimeout(timer); timer = 0; observer.disconnect(); }
function onPageShow() { observe(); schedule(); }
function onVisible() { if (!document.hidden) schedule(); }
const api = {
  set(data, token) {
    onPageHide();
    for (const rule of rules) for (const element of rule.previous) element.removeAttribute(rule.attribute);
    rules = []; attributes.clear(); cursor = 0;
    let css = '';
    const raw = data?.resources?.procedural_actions ?? [];
    for (const encoded of raw.slice(0, 1024)) {
      try {
        const rule = JSON.parse(encoded);
        rule.compiled = compileProceduralSelector(rule.selector);
        rule.previous = [];
        rule.attribute = 'data-lumie-' + token + '-' + rules.length;
        attributes.add(rule.attribute);
        rules.push(rule);
        if (!rule.action || rule.action.type === 'style') {
          css += '[' + rule.attribute + ']{' + (rule.action?.arg ?? 'display:none!important') + '}\n';
        }
      } catch { /* Keep valid rules when one selector fails to compile. */ }
    }
    observe(); schedule();
    return css;
  },
  stop() {
    alive = false; onPageHide();
    for (const rule of rules) for (const element of rule.previous) element.removeAttribute(rule.attribute);
    rules = [];
    removeEventListener('pagehide', onPageHide);
    removeEventListener('pageshow', onPageShow);
    removeEventListener('popstate', schedule);
    removeEventListener('hashchange', schedule);
    document.removeEventListener('visibilitychange', onVisible);
  },
};
addEventListener('pagehide', onPageHide);
addEventListener('pageshow', onPageShow);
addEventListener('popstate', schedule);
addEventListener('hashchange', schedule);
document.addEventListener('visibilitychange', onVisible);
globalThis.__lumieAdblock = api;
