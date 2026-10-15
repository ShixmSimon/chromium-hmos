// Copyright 2026. SPDX-License-Identifier: MPL-2.0
//! Exercises the shipped C ABI, not a second filter implementation.
use serde_json::{Value, json};
use std::{
    ffi::CStr,
    io::{self, Read},
    time::Instant,
};
fn main() {
    let mut input = String::new();
    io::stdin().read_to_string(&mut input).unwrap();
    let v: Value = serde_json::from_str(&input).unwrap();
    let config = serde_json::to_vec(&v["config"]).unwrap();
    let start = Instant::now();
    unsafe {
        let mut info = std::ptr::null_mut();
        let handle = lumie_adblock::lumie_adblock_compile(config.as_ptr(), config.len(), &mut info);
        let meta: Value = serde_json::from_str(CStr::from_ptr(info).to_str().unwrap()).unwrap();
        lumie_adblock::lumie_adblock_free_string(info);
        assert!(!handle.is_null(), "{meta}");
        let compile_ms = start.elapsed().as_secs_f64() * 1000.;
        let mut results = Vec::new();
        let mut times = Vec::new();
        for q in v["queries"].as_array().unwrap() {
            let bytes = serde_json::to_vec(q).unwrap();
            let start = Instant::now();
            let raw = lumie_adblock::lumie_adblock_query(handle, bytes.as_ptr(), bytes.len());
            times.push(start.elapsed().as_secs_f64() * 1e6);
            results.push(
                serde_json::from_str::<Value>(CStr::from_ptr(raw).to_str().unwrap()).unwrap(),
            );
            lumie_adblock::lumie_adblock_free_string(raw);
        }
        lumie_adblock::lumie_adblock_free(handle);
        println!(
            "{}",
            json!({"meta":meta,"compileMs":compile_ms,"queryUs":times,"results":results})
        );
    }
}
