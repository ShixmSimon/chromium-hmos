// Copyright 2026. SPDX-License-Identifier: MPL-2.0
//! Deliberately no include fetching: subscription IO remains browser-owned.
//! Unknown conditions exclude both branches, including their negations.
#[derive(Default)]
pub struct Preprocessor {
    stack: Vec<(Option<bool>, bool)>,
}
impl Preprocessor {
    pub fn line(&mut self, line: &str) -> Result<Option<bool>, ()> {
        if let Some(expr) = line.strip_prefix("!#if ") {
            if self.stack.len() == 32 || expr.len() > 1024 {
                return Err(());
            }
            self.stack.push((condition(expr.trim()), false));
            return Ok(None);
        }
        if line == "!#else" {
            let (value, seen) = self.stack.last_mut().ok_or(())?;
            if *seen {
                return Err(());
            }
            *seen = true;
            *value = value.map(|v| !v);
            return Ok(None);
        }
        if line == "!#endif" {
            self.stack.pop().ok_or(())?;
            return Ok(None);
        }
        if line.starts_with("!#") {
            return Ok(Some(false));
        }
        Ok(Some(
            self.stack.iter().all(|(value, _)| *value == Some(true)),
        ))
    }
    pub fn unknown(&self) -> bool {
        self.stack.iter().any(|(v, _)| v.is_none())
    }
    pub fn complete(&self) -> bool {
        self.stack.is_empty()
    }
}
fn symbol(expr: &str) -> Option<bool> {
    // These are capability statements, not spoofed browser identities.
    Some(match expr {
        "env_chromium" | "cap_user_stylesheet" => true,
        "ext_ublock" | "ext_ubol" | "ext_abp" | "env_firefox" | "env_edge" | "env_safari"
        | "env_legacy" | "env_mv3" | "env_devbuild" | "ext_devbuild" | "cap_html_filtering"
        | "adguard" | "false" => false,
        // A profile can run on phone and tablet, so no platform-specific rules.
        _ => return None,
    })
}
fn condition(expr: &str) -> Option<bool> {
    struct Parser<'a> {
        text: &'a str,
        depth: usize,
    }
    impl Parser<'_> {
        fn eat(&mut self, token: &str) -> bool {
            self.text = self.text.trim_start();
            if let Some(rest) = self.text.strip_prefix(token) {
                self.text = rest;
                true
            } else {
                false
            }
        }
        fn atom(&mut self) -> Option<bool> {
            self.depth += 1;
            if self.depth > 32 {
                return None;
            }
            let value = if self.eat("!") {
                self.atom().map(|v| !v)
            } else if self.eat("(") {
                let value = self.or();
                if !self.eat(")") {
                    return None;
                }
                value
            } else {
                self.text = self.text.trim_start();
                let len = self
                    .text
                    .bytes()
                    .take_while(|c| c.is_ascii_alphanumeric() || *c == b'_')
                    .count();
                if len == 0 {
                    return None;
                }
                let value = symbol(&self.text[..len]);
                self.text = &self.text[len..];
                value
            };
            self.depth -= 1;
            value
        }
        fn and(&mut self) -> Option<bool> {
            let mut v = self.atom();
            while self.eat("&&") {
                let rhs = self.atom();
                v = v.zip(rhs).map(|(a, b)| a && b);
            }
            v
        }
        fn or(&mut self) -> Option<bool> {
            let mut v = self.and();
            while self.eat("||") {
                let rhs = self.and();
                v = v.zip(rhs).map(|(a, b)| a || b);
            }
            v
        }
    }
    let mut p = Parser {
        text: expr,
        depth: 0,
    };
    let value = p.or();
    if p.text.trim().is_empty() {
        value
    } else {
        None
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn capability_and_unknown_negation() {
        let mut p = Preprocessor::default();
        assert_eq!(p.line("!#if env_chromium"), Ok(None));
        assert_eq!(p.line("rule"), Ok(Some(true)));
        p.line("!#if !unknown").unwrap();
        assert_eq!(p.line("rule"), Ok(Some(false)));
        p.line("!#else").unwrap();
        assert_eq!(p.line("rule"), Ok(Some(false)));
        p.line("!#endif").unwrap();
        p.line("!#else").unwrap();
        assert_eq!(p.line("rule"), Ok(Some(false)));
        p.line("!#endif").unwrap();
        assert!(p.complete());
        assert!(p.line("!#endif").is_err());
    }
}
