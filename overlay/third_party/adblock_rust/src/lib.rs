// Copyright 2026. SPDX-License-Identifier: MPL-2.0
//! C ABI for Lumie's immutable adblock-rust snapshots. No IO, JavaScript or URL logging.
use adblock::{
    Engine, FilterSet,
    filters::cosmetic::CosmeticFilterMask,
    lists::{ParseOptions, ParsedLine, parse_filter},
    request::Request,
};
use serde::Deserialize;
use serde_json::{Value, json};
use std::{
    collections::BTreeSet,
    ffi::{CString, c_char},
    panic::{AssertUnwindSafe, catch_unwind},
};

const MAX_SOURCE: usize = 16 * 1024 * 1024;
const MAX_TOTAL: usize = 64 * 1024 * 1024;
const MAX_RULES: usize = 500_000;

#[derive(Deserialize)]
struct Source {
    id: String,
    text: String,
    #[serde(default)]
    strict: bool,
    #[serde(default)]
    bundled: bool,
}
#[derive(Deserialize)]
struct Input {
    sources: Vec<Source>,
}
struct Snapshot {
    engine: Engine,
    ids: Vec<String>,
    generic: BTreeSet<String>,
}

fn build(input: Input) -> Result<(Snapshot, Value), Value> {
    if input.sources.len() > 33
        || input.sources.iter().map(|s| s.text.len()).sum::<usize>() > MAX_TOTAL
    {
        return Err(json!({"ok":false,"reason":"limit"}));
    }
    let mut filters = FilterSet::new(true);
    let mut ids = Vec::new();
    let mut counts = Vec::new();
    let mut generic = BTreeSet::new();
    let mut total = 0;
    for mut source in input.sources {
        if source.bundled && source.text.is_empty() {
            source.text = match source.id.as_str() {
                "easylist" => include_str!("../lists/easylist.txt"),
                "easylist-china" => include_str!("../lists/easylist-china.txt"),
                "easyprivacy" => include_str!("../lists/easyprivacy.txt"),
                _ => "",
            }
            .to_owned();
        }
        let head = source
            .text
            .chars()
            .take(256)
            .collect::<String>()
            .to_ascii_lowercase();
        if head.contains("<!doctype html") || head.contains("<html") {
            return Err(json!({"ok":false,"reason":"invalidRules","errorLine":1}));
        }
        if source.text.len() > MAX_SOURCE || source.text.contains('\0') {
            return Err(json!({"ok":false,"reason":"invalidRules","errorLine":1}));
        }
        let mut accepted = String::new();
        let mut count = 0;
        let mut unsupported = 0;
        // Preprocessor blocks cannot be evaluated as unconditional rules.
        let mut conditional_depth: usize = 0;
        for (line_number, raw) in source.text.lines().enumerate() {
            let line = raw.trim().trim_start_matches('\u{feff}');
            if line.starts_with("!#if") {
                conditional_depth += 1;
            }
            if line.starts_with("!#endif") {
                conditional_depth = conditional_depth.saturating_sub(1);
                continue;
            }
            if line.is_empty() || line.starts_with('!') || line.starts_with("[Adblock") {
                continue;
            }
            total += 1;
            if total > MAX_RULES {
                return Err(json!({"ok":false,"reason":"limit"}));
            }
            let parsed = if line.len() <= 16384 && conditional_depth == 0 {
                parse_filter(line, false, ParseOptions::default()).ok()
            } else {
                None
            };
            let supported = match parsed {
                Some(ParsedLine::Network(n)) => {
                    // No trusted redirect/scriptlet resource pack, response rewriting or CSP yet.
                    !n.is_redirect() && !n.is_csp() && !n.is_removeparam()
                }
                Some(ParsedLine::Cosmetic(c)) => {
                    if let Some(selector) = c.plain_css_selector() {
                        // Defence in depth: this is concatenated into a stylesheet, never code.
                        let safe = c.action.is_none()
                            && !c.mask.contains(CosmeticFilterMask::SCRIPT_INJECT)
                            && !selector.contains(['{', '}'])
                            && !selector.contains("/*");
                        if safe
                            && !c.mask.contains(CosmeticFilterMask::UNHIDE)
                            && (!c.has_hostname_constraint() || c.hidden_generic_rule().is_some())
                        {
                            generic.insert(selector.to_owned());
                        }
                        safe
                    } else {
                        false
                    }
                }
                None => false,
            };
            if supported {
                accepted.push_str(line);
                accepted.push('\n');
                count += 1;
            } else {
                unsupported += 1;
                if source.strict {
                    return Err(
                        json!({"ok":false,"reason":"invalidRules","errorLine":line_number+1}),
                    );
                }
            }
        }
        counts.push(json!({"id":source.id,"ruleCount":count,"unsupportedCount":unsupported}));
        ids.push(source.id);
        filters.add_filter_list(accepted, ParseOptions::default());
    }
    // Preserve source attribution; optimising can fuse rules from several lists.
    let engine = Engine::new_with_filter_set_no_optimize(filters);
    Ok((
        Snapshot {
            engine,
            ids,
            generic,
        },
        json!({"ok":true,"counts":counts}),
    ))
}

fn query(snapshot: &Snapshot, v: Value) -> Value {
    let get = |key| v.get(key).and_then(Value::as_str).unwrap_or("");
    let url = get("url");
    if get("op") == "cosmetic" {
        let r = snapshot.engine.url_cosmetic_resources(url);
        let mut selectors: BTreeSet<_> = r.hide_selectors.into_iter().collect();
        if !r.generichide {
            selectors.extend(
                snapshot
                    .generic
                    .iter()
                    .filter(|s| !r.exceptions.contains(*s))
                    .cloned(),
            );
        }
        let css: String = selectors
            .into_iter()
            .filter(|s| !s.contains(['{', '}']) && !s.contains("/*"))
            .map(|s| format!("{s}{{display:none!important;}}\n"))
            .collect();
        if css.len() > 4 * 1024 * 1024 {
            return json!({"css":""});
        }
        return json!({"css":css});
    }
    let Ok(request) = Request::new(url, get("source"), get("type"), get("method")) else {
        return json!({"blocked":false});
    };
    let result = snapshot.engine.check_network_request(&request);
    let source = result
        .filter
        .as_ref()
        .and_then(|f| f.source_location.as_ref())
        .and_then(|l| snapshot.ids.get(l.source_index as usize))
        .map(String::as_str)
        .unwrap_or("");
    json!({"blocked":result.should_block(),"sourceId":source})
}

fn output(value: Value) -> *mut c_char {
    CString::new(value.to_string()).unwrap().into_raw()
}
unsafe fn input<'a>(data: *const u8, len: usize) -> Option<&'a [u8]> {
    if data.is_null() || len > MAX_TOTAL + 1024 * 1024 {
        return None;
    }
    Some(unsafe { std::slice::from_raw_parts(data, len) })
}

// All returned strings and snapshots belong to this library. The caller must use the
// corresponding free function, not Chromium's allocator. Calls may run on workers.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lumie_adblock_compile(
    data: *const u8,
    len: usize,
    info: *mut *mut c_char,
) -> *mut std::ffi::c_void {
    if info.is_null() {
        return std::ptr::null_mut();
    }
    let result = catch_unwind(AssertUnwindSafe(|| {
        let bytes = unsafe { input(data, len) }.ok_or(json!({"ok":false,"reason":"limit"}))?;
        let parsed = serde_json::from_slice(bytes)
            .map_err(|_| json!({"ok":false,"reason":"invalidRules","errorLine":1}))?;
        build(parsed)
    }))
    .unwrap_or_else(|_| Err(json!({"ok":false,"reason":"compileFailed"})));
    match result {
        Ok((snapshot, meta)) => {
            unsafe {
                *info = output(meta);
            }
            Box::into_raw(Box::new(snapshot)).cast()
        }
        Err(error) => {
            unsafe {
                *info = output(error);
            }
            std::ptr::null_mut()
        }
    }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lumie_adblock_query(
    handle: *const std::ffi::c_void,
    data: *const u8,
    len: usize,
) -> *mut c_char {
    let result = catch_unwind(AssertUnwindSafe(|| {
        if handle.is_null() {
            return json!({});
        }
        let Some(bytes) = (unsafe { input(data, len) }) else {
            return json!({});
        };
        let Ok(v) = serde_json::from_slice(bytes) else {
            return json!({});
        };
        query(unsafe { &*handle.cast::<Snapshot>() }, v)
    }))
    .unwrap_or_else(|_| json!({}));
    output(result)
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lumie_adblock_free(handle: *mut std::ffi::c_void) {
    if !handle.is_null() {
        drop(unsafe { Box::from_raw(handle.cast::<Snapshot>()) });
    }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lumie_adblock_free_string(s: *mut c_char) {
    if !s.is_null() {
        drop(unsafe { CString::from_raw(s) });
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn snapshot(rules: &str) -> Snapshot {
        build(Input {
            sources: vec![Source {
                id: "test".into(),
                text: rules.into(),
                strict: true,
                bundled: false,
            }],
        })
        .unwrap()
        .0
    }
    fn blocked(s: &Snapshot, url: &str, source: &str, kind: &str) -> bool {
        query(
            s,
            json!({"url":url,"source":source,"type":kind,"method":"GET"}),
        )["blocked"]
            .as_bool()
            .unwrap()
    }
    #[test]
    fn network_types_third_party_and_exceptions() {
        let s = snapshot("||ads.test^$script,third-party\n@@||ads.test/allowed.js$script");
        assert!(blocked(
            &s,
            "https://ads.test/ad.js",
            "https://site.test",
            "script"
        ));
        assert!(!blocked(
            &s,
            "https://ads.test/ad.js",
            "https://ads.test",
            "script"
        ));
        assert!(!blocked(
            &s,
            "https://ads.test/ad.js",
            "https://site.test",
            "image"
        ));
        assert!(!blocked(
            &s,
            "https://ads.test/allowed.js",
            "https://site.test",
            "script"
        ));
    }
    #[test]
    fn source_attribution_and_cross_source_allow() {
        let (s, _) = build(Input {
            sources: vec![
                Source {
                    id: "block".into(),
                    text: "||ads.test^".into(),
                    strict: true,
                    bundled: false,
                },
                Source {
                    id: "allow".into(),
                    text: "@@||ads.test/allowed".into(),
                    strict: true,
                    bundled: false,
                },
            ],
        })
        .unwrap();
        let v = query(
            &s,
            json!({"url":"https://ads.test/a","source":"https://site.test","type":"image","method":"get"}),
        );
        assert_eq!(v["sourceId"], "block");
        assert!(!blocked(
            &s,
            "https://ads.test/allowed",
            "https://site.test",
            "image"
        ));
    }
    #[test]
    fn cosmetics_generic_domain_exception_and_negation() {
        let s =
            snapshot("##.advert\nsite.test#@#.advert\n~except.test##.negated\nsite.test##.local");
        let css = |url| {
            query(&s, json!({"op":"cosmetic","url":url}))["css"]
                .as_str()
                .unwrap()
                .to_owned()
        };
        assert!(css("https://other.test").contains(".advert{"));
        assert!(!css("https://site.test").contains(".advert{"));
        assert!(css("https://site.test").contains(".local{"));
        assert!(!css("https://except.test").contains(".negated{"));
    }
    #[test]
    fn unsupported_cannot_turn_into_blocking_rules() {
        let (s,m)=build(Input{sources:vec![Source{id:"test".into(),text:"||ads.test^$redirect=noopjs\nsite.test##+js(abort-on-property-read, ads)\nsite.test##.ad:remove()\n||ads.test^$removeparam=ad".into(),strict:false,bundled:false}]}).unwrap();
        assert_eq!(m["counts"][0]["unsupportedCount"], 4);
        assert!(!blocked(
            &s,
            "https://ads.test/a",
            "https://site.test",
            "script"
        ));
    }
    #[test]
    fn invalid_user_line_is_reported() {
        let e = build(Input {
            sources: vec![Source {
                id: "user".into(),
                text: "! comment\n||ads.test^\nsite.test##+js(foo)".into(),
                strict: true,
                bundled: false,
            }],
        })
        .err()
        .unwrap();
        assert_eq!(e["errorLine"], 3);
    }
    #[test]
    fn snapshots_are_send_sync() {
        fn check<T: Send + Sync>() {}
        check::<Snapshot>();
    }
    #[test]
    fn offline_bundled_lists_compile_and_block() {
        let sources = ["easylist", "easylist-china"]
            .into_iter()
            .map(|id| Source {
                id: id.into(),
                text: String::new(),
                strict: false,
                bundled: true,
            })
            .collect();
        let (s, m) = build(Input { sources }).unwrap();
        assert!(m["counts"][0]["ruleCount"].as_u64().unwrap() > 10000);
        assert!(m["counts"][1]["ruleCount"].as_u64().unwrap() > 1000);
        assert!(blocked(
            &s,
            "https://pagead2.googlesyndication.com/pagead/js/adsbygoogle.js",
            "https://example.org",
            "script"
        ));
        assert!(!blocked(
            &s,
            "https://example.org/article",
            "https://example.org",
            "document"
        ));
    }
    #[test]
    fn html_error_pages_are_not_rules() {
        assert!(
            build(Input {
                sources: vec![Source {
                    id: "x".into(),
                    text: "<!DOCTYPE html><html>error</html>".into(),
                    strict: false,
                    bundled: false
                }]
            })
            .is_err()
        );
    }
    #[test]
    fn conditional_filters_are_not_unconditional() {
        let (s, m) = build(Input {
            sources: vec![Source {
                id: "x".into(),
                text: "!#if ext_ublock\n||example.org^\n!#endif".into(),
                strict: false,
                bundled: false,
            }],
        })
        .unwrap();
        assert_eq!(m["counts"][0]["unsupportedCount"], 1);
        assert!(!blocked(
            &s,
            "https://example.org/a",
            "https://site.test",
            "script"
        ));
    }
    #[test]
    fn old_snapshot_survives_new_invalid_rules() {
        let s = snapshot("||ads.test^");
        assert!(
            build(Input {
                sources: vec![Source {
                    id: "x".into(),
                    text: "site.test##+js(foo)".into(),
                    strict: true,
                    bundled: false
                }]
            })
            .is_err()
        );
        assert!(blocked(
            &s,
            "https://ads.test/a",
            "https://site.test",
            "image"
        ));
    }
}
