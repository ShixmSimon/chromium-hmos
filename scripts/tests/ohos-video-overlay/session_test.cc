#include <cassert>
#include <cstdio>
#include <limits>
#include "ui/ozone/platform/ohos/ohos_overlay_session.h"

using namespace ui::ohos_overlay;
struct Lease : ReadLease {
  uint64_t id;
  int& releases;
  bool& producer;
  Lease(uint64_t value, int& count, bool& ready)
      : id(value), releases(count), producer(ready) {}
  ~Lease() override { ++releases; }
  uint64_t allocation_id() const override { return id; }
  void* native_buffer() const override {
    return reinterpret_cast<void*>(id + 1);
  }
  int BorrowAcquireFence() const override { return -1; }
  bool ProducerReady() const override { return producer; }
};
struct FakeBackend : Backend {
  struct Batch : Prepared {};
  bool supported = true, fail_prepare = false, ack = false, drained = false;
  bool ready_for_frame = true;
  int preparations = 0, commits = 0, drains = 0, destroys = 0;
  std::vector<Release> releases;
  bool Supports(const Frame&) const override { return supported; }
  bool ReadyForFrame() const override { return ready_for_frame; }
  std::unique_ptr<Prepared> Prepare(uint64_t, const Frame&) override {
    ++preparations;
    return fail_prepare ? nullptr : std::make_unique<Batch>();
  }
  void Commit(std::unique_ptr<Prepared>) override { ++commits; }
  std::vector<Release> PollReleases() override {
    auto result = std::move(releases);
    releases.clear();
    return result;
  }
  bool CommandsProcessed(uint64_t) const override { return ack; }
  void BeginDrain() override {
    assert(ack || commits == 0);
    ++drains;
  }
  bool DrainCommandsProcessed() const override { return drained; }
  void DestroyLayers() override {
    assert(drained);
    ++destroys;
  }
};
Plane MakePlane(uint64_t id, int& released, bool& ready, bool video) {
  Plane p;
  p.lease = std::make_unique<Lease>(id, released, ready);
  p.width = 1280;
  p.height = 720;
  p.source = {0, 0, 1280, 720};
  p.destination = {100, 200, 640, 360};
  p.format = video ? Format::kNv12 : Format::kRgba8;
  return p;
}
Frame MakeFrame(uint64_t first, int& released, bool& ready, bool video = true) {
  Frame f;
  f.primary = MakePlane(first, released, ready, false);
  if (video)
    f.video = MakePlane(first + 1, released, ready, true);
  f.primary_has_video_hole = video;
  return f;
}
void Finish(Session& session, FakeBackend& backend) {
  backend.ack = true;
  backend.drained = true;
  session.Close();
  assert(session.closed());
}
int main() {
  bool ready = true;
  int released = 0;
  {
    FakeBackend b;
    Session s(b);
    auto f = MakeFrame(10, released, ready);
    f.primary_has_video_hole = false;
    assert(s.Submit(f) == SubmitResult::kUnsupported && b.preparations == 0);
    f.primary_has_video_hole = true;
    f.video->destination.x = -1;
    assert(s.Submit(f) == SubmitResult::kUnsupported);
    f.video->destination.x = 100;
    f.video->source.width = std::numeric_limits<float>::quiet_NaN();
    assert(s.Submit(f) == SubmitResult::kUnsupported);
    Finish(s, b);
  }
  released = 0;
  {
    FakeBackend b;
    Session s(b);
    auto f = MakeFrame(10, released, ready);
    b.fail_prepare = true;
    assert(s.Submit(f) == SubmitResult::kPrepareFailed);
    assert(f.primary.lease && f.video->lease && released == 0 &&
           b.commits == 0);
    Finish(s, b);
  }
  released = 0;
  {
    FakeBackend b;
    Session s(b, 1);
    auto f = MakeFrame(10, released, ready);
    assert(s.Submit(f) == SubmitResult::kAccepted && !f.primary.lease);
    auto next = MakeFrame(20, released, ready);
    assert(s.Submit(next) == SubmitResult::kBusy && next.primary.lease);
    b.releases = {
        {1, 10, 1, true, true}, {0, 10, 0, true, true}, {1, 99, 0, true, true}};
    s.Poll();
    assert(released ==
           0);  // Wrong plane, stale generation, foreign allocation.
    b.releases = {{1, 10, 0, false, true}, {1, 11, 1, true, false}};
    s.Poll();
    assert(released == 0);  // Fence alone or callback alone is insufficient.
    b.releases = {{1, 10, 0, true, true}, {1, 11, 1, true, true}};
    ready = false;
    s.Poll();
    assert(released == 0);  // GPU still writing.
    ready = true;
    s.Poll();
    assert(released == 2);
    assert(s.Submit(next) == SubmitResult::kAccepted);
    b.releases = {
        {1, 20, 0, true, true}, {2, 20, 0, true, true}, {2, 21, 1, true, true}};
    s.Poll();
    assert(released == 4);
    Finish(s, b);
    assert(b.destroys == 1);
  }
  released = 0;
  {
    FakeBackend b;
    Session s(b);
    auto f = MakeFrame(30, released, ready);
    assert(s.Submit(f) == SubmitResult::kAccepted);
    s.Close();
    assert(s.state() == State::kWaitingForCommands && b.drains == 0);
    b.ack = true;
    s.Poll();
    assert(b.drains == 1 && !s.closed());
    b.drained = true;
    s.Poll();
    assert(!s.closed());
    b.releases = {{1, 30, 0, true, true}, {1, 31, 1, true, false}};
    s.Poll();
    assert(released == 1 && !s.closed());
    b.releases = {{1, 31, 1, true, true}};
    s.Poll();
    assert(released == 2 && s.closed());
    s.Close();
    s.Poll();
    assert(b.destroys == 1);
    auto next = MakeFrame(32, released, ready);
    assert(s.Submit(next) == SubmitResult::kClosed);
  }
  released = 0;
  {
    FakeBackend b;
    Session s(b, 1);
    auto original = MakeFrame(40, released, ready);
    assert(s.Submit(original) == SubmitResult::kAccepted);
    for (int i = 0; i < 100; ++i) {
      auto paused = MakeFrame(40, released, ready);
      paused.primary.reuse = paused.video->reuse = true;
      paused.video->destination.width = 600;
      paused.video->destination.x += i % 10;
      assert(s.Submit(paused) == SubmitResult::kAccepted);
      assert(s.outstanding_allocations() == 2);
      // Redundant references retire AFTER acceptance; the original read stays.
      s.Poll();
      assert(released == (i + 1) * 2);
    }
    auto changed = MakeFrame(40, released, ready);
    changed.primary.reuse = changed.video->reuse = true;
    changed.video->color.native_space = 5;
    assert(s.Submit(changed) == SubmitResult::kUnsupported);
    b.releases = {{101, 40, 0, true, true}, {101, 41, 1, true, true}};
    s.Poll();
    assert(s.outstanding_allocations() == 2);  // reuse is not a new SetBuffer.
    b.releases = {{1, 40, 0, true, true}, {1, 41, 1, true, true}};
    s.Poll();
    assert(s.outstanding_allocations() == 0);
    Finish(s, b);
  }
  // Demotion drains the old video before re-promotion. A busy retry must not
  // consume either new read lease, and a stale reuse must not resurrect it.
  released = 0;
  {
    FakeBackend b;
    Session s(b);
    auto promoted = MakeFrame(80, released, ready);
    assert(s.Submit(promoted) == SubmitResult::kAccepted);
    auto demoted = MakeFrame(90, released, ready, false);
    assert(s.Submit(demoted) == SubmitResult::kAccepted);
    b.ready_for_frame = false;
    auto next = MakeFrame(100, released, ready);
    assert(s.Submit(next) == SubmitResult::kBusy);
    assert(next.primary.lease && next.video->lease && b.commits == 2);
    b.releases = {{1, 80, 0, true, true}, {1, 81, 1, true, false}};
    s.Poll();
    assert(released == 1);
    b.releases = {{1, 81, 1, true, true}};
    s.Poll();
    assert(released == 2);
    b.ready_for_frame = true;
    auto stale = MakeFrame(80, released, ready);
    stale.primary.reuse = stale.video->reuse = true;
    assert(s.Submit(stale) == SubmitResult::kUnsupported);
    assert(s.Submit(next) == SubmitResult::kAccepted);
    b.releases = {{2, 90, 0, true, true},
                  {3, 100, 0, true, true},
                  {3, 101, 1, true, true}};
    s.Poll();
    assert(released == 5);
    Finish(s, b);
  }
  // Hundreds of recycled allocations: no fixed test-slot/marker limit, and a
  // delayed callback from an earlier use must not retire the current read.
  released = 0;
  {
    FakeBackend b;
    Session s(b);
    for (uint64_t sequence = 1; sequence <= 500; ++sequence) {
      auto f = MakeFrame(50, released, ready);
      if (sequence % 2 == 0) {
        f.video->format = Format::kP010;
        f.video->color.transfer = Transfer::kPq;
        f.video->color.native_space = 1;
        f.video->color.hdr_type = {1};
      }
      assert(s.Submit(f) == SubmitResult::kAccepted);
      b.releases = {{sequence - 1, 50, 0, true, true},
                    {sequence - 1, 51, 1, true, true}};
      s.Poll();
      assert(released == static_cast<int>((sequence - 1) * 2));
      b.releases = {{sequence, 50, 0, true, true},
                    {sequence, 51, 1, true, true}};
      s.Poll();
    }
    assert(released == 1000);
    Finish(s, b);
  }
  {
    FakeBackend b;
    Session s(b);
    auto f = MakeFrame(70, released, ready);
    f.video->color.transfer = Transfer::kPq;
    assert(!Session::Valid(f));  // 8-bit PQ rejected.
    f.video->format = Format::kP010;
    assert(!Session::Valid(f));  // Missing native color/type rejected.
    f.video->color.native_space = 1;
    f.video->color.hdr_type = {1};
    assert(Session::Valid(f));
    f.primary.color.transfer = Transfer::kPq;
    assert(!Session::Valid(f));  // SDR text primary remains independent.
    Finish(s, b);
  }
  puts(
      "PASS overlay pair ownership, fences, async close, stale callbacks, 500 "
      "allocation reuses and HDR policy");
}
