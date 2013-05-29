// Copyright (c) 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/command_line.h"
#include "base/message_loop_proxy.h"
#include "base/path_service.h"
#include "base/run_loop.h"
#include "content/browser/gpu/gpu_data_manager_impl.h"
#include "content/browser/renderer_host/render_widget_host_impl.h"
#include "content/port/browser/render_widget_host_view_frame_subscriber.h"
#include "content/port/browser/render_widget_host_view_port.h"
#include "content/public/browser/compositor_util.h"
#include "content/public/browser/render_view_host.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/content_paths.h"
#include "content/public/common/content_switches.h"
#include "content/shell/shell.h"
#include "content/test/content_browser_test.h"
#include "content/test/content_browser_test_utils.h"
#include "media/base/video_frame.h"
#include "net/base/net_util.h"
#include "ui/compositor/compositor_setup.h"
#if defined(OS_MACOSX)
#include "ui/surface/io_surface_support_mac.h"
#endif

namespace content {
namespace {

// Convenience macro: Short-cicuit a pass for the tests where platform support
// for forced-compositing mode (or disabled-compositing mode) is lacking.
#define SET_UP_SURFACE_OR_PASS_TEST()  \
  if (!SetUpSourceSurface()) {  \
    LOG(WARNING)  \
        << ("Blindly passing this test: This platform does not support "  \
            "forced compositing (or forced-disabled compositing) mode.");  \
    return;  \
  }

// Common base class for browser tests.  This is subclassed twice: Once to test
// the browser in forced-compositing mode, and once to test with compositing
// mode disabled.
class RenderWidgetHostViewBrowserTest : public ContentBrowserTest {
 public:
  RenderWidgetHostViewBrowserTest()
      : frame_size_(400, 300),
        callback_invoke_count_(0),
        frames_captured_(0) {}

  virtual void SetUpInProcessBrowserTestFixture() OVERRIDE {
    ASSERT_TRUE(PathService::Get(DIR_TEST_DATA, &test_dir_));
    ContentBrowserTest::SetUpInProcessBrowserTestFixture();
  }

  // Attempts to set up the source surface.  Returns false if unsupported on the
  // current platform.
  virtual bool SetUpSourceSurface() = 0;

  int callback_invoke_count() const {
    return callback_invoke_count_;
  }

  int frames_captured() const {
    return frames_captured_;
  }

  const gfx::Size& frame_size() const {
    return frame_size_;
  }

  const base::FilePath& test_dir() const {
    return test_dir_;
  }

  RenderViewHost* GetRenderViewHost() const {
    RenderViewHost* const rvh = shell()->web_contents()->GetRenderViewHost();
    CHECK(rvh);
    return rvh;
  }

  RenderWidgetHostImpl* GetRenderWidgetHost() const {
    RenderWidgetHostImpl* const rwh = RenderWidgetHostImpl::From(
        shell()->web_contents()->GetRenderWidgetHostView()->
            GetRenderWidgetHost());
    CHECK(rwh);
    return rwh;
  }

  RenderWidgetHostViewPort* GetRenderWidgetHostViewPort() const {
    RenderWidgetHostViewPort* const view =
        RenderWidgetHostViewPort::FromRWHV(GetRenderViewHost()->GetView());
    CHECK(view);
    return view;
  }

  // Callback when using CopyFromBackingStore() API.
  void FinishCopyFromBackingStore(const base::Closure& quit_closure,
                                  bool frame_captured,
                                  const SkBitmap& bitmap) {
    ++callback_invoke_count_;
    if (frame_captured) {
      ++frames_captured_;
      EXPECT_FALSE(bitmap.empty());
    }
    if (!quit_closure.is_null())
      quit_closure.Run();
  }

  // Callback when using CopyFromCompositingSurfaceToVideoFrame() API.
  void FinishCopyFromCompositingSurface(const base::Closure& quit_closure,
                                        bool frame_captured) {
    ++callback_invoke_count_;
    if (frame_captured)
      ++frames_captured_;
    if (!quit_closure.is_null())
      quit_closure.Run();
  }

  // Callback when using frame subscriber API.
  void FrameDelivered(const scoped_refptr<base::MessageLoopProxy>& loop,
                      base::Closure quit_closure,
                      base::Time timestamp,
                      bool frame_captured) {
    ++callback_invoke_count_;
    if (frame_captured)
      ++frames_captured_;
    if (!quit_closure.is_null())
      loop->PostTask(FROM_HERE, quit_closure);
  }

  // Copy one frame using the CopyFromBackingStore API.
  void RunBasicCopyFromBackingStoreTest() {
    SET_UP_SURFACE_OR_PASS_TEST();

    // Repeatedly call CopyFromBackingStore() since, on some platforms (e.g.,
    // Windows), the operation will fail until the first "present" has been
    // made.
    int count_attempts = 0;
    while (true) {
      ++count_attempts;
      base::RunLoop run_loop;
      GetRenderViewHost()->CopyFromBackingStore(
          gfx::Rect(),
          frame_size(),
          base::Bind(
              &RenderWidgetHostViewBrowserTest::FinishCopyFromBackingStore,
              base::Unretained(this),
              run_loop.QuitClosure()));
      run_loop.Run();

      if (frames_captured())
        break;
      else
        GiveItSomeTime();
    }

    EXPECT_EQ(count_attempts, callback_invoke_count());
    EXPECT_EQ(1, frames_captured());
  }

 protected:
  // Waits until the source is available for copying.
  void WaitForCopySourceReady() {
    while (!GetRenderWidgetHostViewPort()->IsSurfaceAvailableForCopy())
      GiveItSomeTime();
  }

  // Run the current message loop for a short time without unwinding the current
  // call stack.
  static void GiveItSomeTime() {
    base::RunLoop run_loop;
    MessageLoop::current()->PostDelayedTask(
        FROM_HERE,
        run_loop.QuitClosure(),
        base::TimeDelta::FromMilliseconds(10));
    run_loop.Run();
  }

 private:
  const gfx::Size frame_size_;
  base::FilePath test_dir_;
  int callback_invoke_count_;
  int frames_captured_;
};

class CompositingRenderWidgetHostViewBrowserTest
    : public RenderWidgetHostViewBrowserTest {
 public:
  virtual void SetUpCommandLine(CommandLine* command_line) OVERRIDE {
    // Note: Not appending kForceCompositingMode switch here, since not all bots
    // support compositing.  Some bots will run with compositing on, and others
    // won't.  Therefore, the call to SetUpSourceSurface() later on will detect
    // whether compositing mode is actually on or not.  If not, the tests will
    // pass blindly, logging a warning message, since we cannot test what the
    // platform/implementation does not support.
    RenderWidgetHostViewBrowserTest::SetUpCommandLine(command_line);
  }

  virtual bool SetUpSourceSurface() OVERRIDE {
    if (!IsForceCompositingModeEnabled())
      return false;  // See comment in SetUpCommandLine().
#if defined(OS_MACOSX)
    CHECK(IOSurfaceSupport::Initialize());
#endif
    NavigateToURL(shell(), net::FilePathToFileURL(
        test_dir().AppendASCII("rwhv_compositing_animation.html")));
#if !defined(USE_AURA)
    if (!GetRenderWidgetHost()->is_accelerated_compositing_active())
      return false;  // Renderer did not turn on accelerated compositing.
#endif

    // Using accelerated compositing, but a compositing surface might not be
    // available yet.  So, wait for it.
    WaitForCopySourceReady();
    return true;
  }
};

class NonCompositingRenderWidgetHostViewBrowserTest
    : public RenderWidgetHostViewBrowserTest {
 public:
  virtual void SetUpCommandLine(CommandLine* command_line) OVERRIDE {
    // Note: Appending the kDisableAcceleratedCompositing switch here, but there
    // are some builds that only use compositing and will ignore this switch.
    // Therefore, the call to SetUpSourceSurface() later on will detect whether
    // compositing mode is actually off.  If it's on, the tests will pass
    // blindly, logging a warning message, since we cannot test what the
    // platform/implementation does not support.
    command_line->AppendSwitch(switches::kDisableAcceleratedCompositing);
    RenderWidgetHostViewBrowserTest::SetUpCommandLine(command_line);
  }

  virtual bool SetUpSourceSurface() OVERRIDE {
    if (IsForceCompositingModeEnabled())
      return false;  // See comment in SetUpCommandLine().
    NavigateToURL(shell(), GURL("about:blank"));
    WaitForCopySourceReady();
    // Return whether the renderer left accelerated compositing turned off.
    return !GetRenderWidgetHost()->is_accelerated_compositing_active();
  }
};

class FakeFrameSubscriber : public RenderWidgetHostViewFrameSubscriber {
 public:
  FakeFrameSubscriber(
    RenderWidgetHostViewFrameSubscriber::DeliverFrameCallback callback)
      : callback_(callback) {
  }

  virtual bool ShouldCaptureFrame(
      base::Time present_time,
      scoped_refptr<media::VideoFrame>* storage,
      DeliverFrameCallback* callback) OVERRIDE {
    // Only allow one frame capture to be made.  Otherwise, the compositor could
    // start multiple captures, unbounded, and eventually its own limiter logic
    // will begin invoking |callback| with a |false| result.  This flakes out
    // the unit tests, since they receive a "failed" callback before the later
    // "success" callbacks.
    if (callback_.is_null())
      return false;
    *storage = media::VideoFrame::CreateBlackFrame(gfx::Size(100, 100));
    *callback = callback_;
    callback_.Reset();
    return true;
  }

 private:
  DeliverFrameCallback callback_;
};

// Disable tests for Android and IOS as these platforms have incomplete
// implementation.
#if !defined(OS_ANDROID) && !defined(OS_IOS)

// The CopyFromBackingStore() API should work on all platforms when compositing
// is enabled.
IN_PROC_BROWSER_TEST_F(CompositingRenderWidgetHostViewBrowserTest,
                       CopyFromBackingStore) {
  RunBasicCopyFromBackingStoreTest();
}

// The CopyFromBackingStore() API should work on all platforms when compositing
// is disabled.
IN_PROC_BROWSER_TEST_F(NonCompositingRenderWidgetHostViewBrowserTest,
                       CopyFromBackingStore) {
  RunBasicCopyFromBackingStoreTest();
}

// Tests that the callback passed to CopyFromBackingStore is always called,
// even when the RenderWidgetHost is deleting in the middle of an async copy.
IN_PROC_BROWSER_TEST_F(CompositingRenderWidgetHostViewBrowserTest,
                       CopyFromBackingStore_CallbackDespiteDelete) {
  SET_UP_SURFACE_OR_PASS_TEST();

  base::RunLoop run_loop;
  GetRenderViewHost()->CopyFromBackingStore(
      gfx::Rect(),
      frame_size(),
      base::Bind(&RenderWidgetHostViewBrowserTest::FinishCopyFromBackingStore,
                 base::Unretained(this), run_loop.QuitClosure()));
  // Delete the surface before the callback is run.
  GetRenderWidgetHostViewPort()->AcceleratedSurfaceRelease();
  run_loop.Run();

  EXPECT_EQ(1, callback_invoke_count());
}

// Tests that the callback passed to CopyFromCompositingSurfaceToVideoFrame is
// always called, even when the RenderWidgetHost is deleting in the middle of
// an async copy.
IN_PROC_BROWSER_TEST_F(CompositingRenderWidgetHostViewBrowserTest,
                       CopyFromCompositingSurface_CallbackDespiteDelete) {
  SET_UP_SURFACE_OR_PASS_TEST();
  RenderWidgetHostViewPort* const view = GetRenderWidgetHostViewPort();
  if (!view->CanCopyToVideoFrame()) {
    LOG(WARNING) <<
        ("Blindly passing this test: CopyFromCompositingSurfaceToVideoFrame() "
         "not supported on this platform.");
    return;
  }

  base::RunLoop run_loop;
  scoped_refptr<media::VideoFrame> dest =
      media::VideoFrame::CreateBlackFrame(frame_size());
  view->CopyFromCompositingSurfaceToVideoFrame(
      gfx::Rect(view->GetViewBounds().size()), dest, base::Bind(
          &RenderWidgetHostViewBrowserTest::FinishCopyFromCompositingSurface,
          base::Unretained(this), run_loop.QuitClosure()));
  // Delete the surface before the callback is run.
  view->AcceleratedSurfaceRelease();
  run_loop.Run();

  EXPECT_EQ(1, callback_invoke_count());
}

// With compositing turned off, no platforms should support the
// CopyFromCompositingSurfaceToVideoFrame() API.
IN_PROC_BROWSER_TEST_F(NonCompositingRenderWidgetHostViewBrowserTest,
                       CopyFromCompositingSurfaceToVideoFrameCallbackTest) {
  SET_UP_SURFACE_OR_PASS_TEST();
  EXPECT_FALSE(GetRenderWidgetHostViewPort()->CanCopyToVideoFrame());
}

// Test basic frame subscription functionality.  We subscribe, and then run
// until at least one DeliverFrameCallback has been invoked.
IN_PROC_BROWSER_TEST_F(CompositingRenderWidgetHostViewBrowserTest,
                       FrameSubscriberTest) {
  SET_UP_SURFACE_OR_PASS_TEST();
  RenderWidgetHostViewPort* const view = GetRenderWidgetHostViewPort();
  if (!view->CanSubscribeFrame()) {
    LOG(WARNING) << ("Blindly passing this test: Frame subscription not "
                     "supported on this platform.");
    return;
  }
#if defined(USE_AURA)
  if (ui::IsTestCompositorEnabled()) {
    LOG(WARNING) << ("Blindly passing this test: Aura test compositor doesn't "
                     "support frame subscription.");
    // TODO(miu): Aura test compositor should support frame subscription for
    // testing.  http://crbug.com/240572
    return;
  }
#endif

  base::RunLoop run_loop;
  scoped_ptr<RenderWidgetHostViewFrameSubscriber> subscriber(
      new FakeFrameSubscriber(
          base::Bind(&RenderWidgetHostViewBrowserTest::FrameDelivered,
                     base::Unretained(this),
                     base::MessageLoopProxy::current(),
                     run_loop.QuitClosure())));
  view->BeginFrameSubscription(subscriber.Pass());
  run_loop.Run();
  view->EndFrameSubscription();

  EXPECT_LE(1, callback_invoke_count());
  EXPECT_LE(1, frames_captured());
}

// Test that we can copy twice from an accelerated composited page.
IN_PROC_BROWSER_TEST_F(CompositingRenderWidgetHostViewBrowserTest, CopyTwice) {
  SET_UP_SURFACE_OR_PASS_TEST();
  RenderWidgetHostViewPort* const view = GetRenderWidgetHostViewPort();
  if (!view->CanCopyToVideoFrame()) {
    LOG(WARNING) << ("Blindly passing this test: "
                     "CopyFromCompositingSurfaceToVideoFrame() not supported "
                     "on this platform.");
    return;
  }

  base::RunLoop run_loop;
  scoped_refptr<media::VideoFrame> first_output =
      media::VideoFrame::CreateBlackFrame(frame_size());
  ASSERT_TRUE(first_output);
  scoped_refptr<media::VideoFrame> second_output =
      media::VideoFrame::CreateBlackFrame(frame_size());
  ASSERT_TRUE(second_output);
  view->CopyFromCompositingSurfaceToVideoFrame(
      gfx::Rect(view->GetViewBounds().size()), first_output,
      base::Bind(&RenderWidgetHostViewBrowserTest::FrameDelivered,
                 base::Unretained(this),
                 base::MessageLoopProxy::current(),
                 base::Closure(),
                 base::Time::Now()));
  view->CopyFromCompositingSurfaceToVideoFrame(
      gfx::Rect(view->GetViewBounds().size()), second_output,
      base::Bind(&RenderWidgetHostViewBrowserTest::FrameDelivered,
                 base::Unretained(this),
                 base::MessageLoopProxy::current(),
                 run_loop.QuitClosure(),
                 base::Time::Now()));
  run_loop.Run();

  EXPECT_EQ(2, callback_invoke_count());
  EXPECT_EQ(2, frames_captured());
}

#endif  // !defined(OS_ANDROID) && !defined(OS_IOS)

}  // namespace
}  // namespace content