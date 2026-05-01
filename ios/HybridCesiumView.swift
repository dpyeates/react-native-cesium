import Foundation
import Metal
import MetalKit
import NitroModules
import UIKit

class HybridCesiumView: HybridCesiumViewSpec {
  // MARK: - Props

  var ionAccessToken: String = "" {
    didSet {
      let token = ionAccessToken
      let assetId = Int64(ionAssetId)
      postToRender { [weak self] in self?.bridge?.updateIonAccessToken(token, assetId: assetId) }
    }
  }
  var ionAssetId: Double = 1 {
    didSet {
      let token = ionAccessToken
      let assetId = Int64(ionAssetId)
      postToRender { [weak self] in self?.bridge?.updateIonAccessToken(token, assetId: assetId) }
    }
  }

  /// Construction-time seed camera. Used exactly once when the native bridge
  /// is created; subsequent prop writes are ignored. Use `setCamera(_:)` to
  /// drive the camera at runtime — it does NOT mutate this value.
  var initialCamera: CameraState = CameraState(
    latitude: 46.15,
    longitude: 7.35,
    altitude: 12000,
    heading: 129,
    pitch: -45,
    roll: 0,
    verticalFovDeg: 60
  )
  var pauseRendering: Bool = false {
    didSet {
      displayLink?.isPaused = pauseRendering // UIKit-ish, must stay on main
      if !pauseRendering {
        postToRender { [weak self] in self?.bridge?.markNeedsRender() }
      }
    }
  }

  var maximumScreenSpaceError: Double = 32 {
    didSet { let v = maximumScreenSpaceError; postToRender { [weak self] in self?.bridge?.setMaximumScreenSpaceError(v) } }
  }
  var maximumSimultaneousTileLoads: Double = 12 {
    didSet { let v = Int32(maximumSimultaneousTileLoads); postToRender { [weak self] in self?.bridge?.setMaximumSimultaneousTileLoads(v) } }
  }
  var loadingDescendantLimit: Double = 20 {
    didSet { let v = Int32(loadingDescendantLimit); postToRender { [weak self] in self?.bridge?.setLoadingDescendantLimit(v) } }
  }
  var msaaSampleCount: Double = 1 {
    didSet {
      let s = Int32(msaaSampleCount)
      // applyMtkViewMsaa touches MTKView/UIKit so it stays on main.
      applyMtkViewMsaa(Int(s))
      postToRender { [weak self] in self?.bridge?.setMsaaSampleCount(s) }
    }
  }
  var ionImageryAssetId: Double = 1 {
    didSet { let v = Int64(ionImageryAssetId); postToRender { [weak self] in self?.bridge?.updateImageryAssetId(v) } }
  }

  // ── Optional perf / quality knobs ────────────────────────────────────────
  var maximumCachedMiB: Double? {
    didSet { if let v = maximumCachedMiB { postToRender { [weak self] in self?.bridge?.setMaximumCachedMiB(Int32(v)) } } }
  }
  var preloadAncestors: Bool? {
    didSet { if let v = preloadAncestors { postToRender { [weak self] in self?.bridge?.setPreloadAncestors(v) } } }
  }
  var preloadSiblings: Bool? {
    didSet { if let v = preloadSiblings { postToRender { [weak self] in self?.bridge?.setPreloadSiblings(v) } } }
  }
  var forbidHoles: Bool? {
    didSet { if let v = forbidHoles { postToRender { [weak self] in self?.bridge?.setForbidHoles(v) } } }
  }
  var enableWaterMask: Bool? {
    didSet { if let v = enableWaterMask { postToRender { [weak self] in self?.bridge?.setEnableWaterMask(v) } } }
  }
  var enableFogCulling: Bool? {
    didSet { if let v = enableFogCulling { postToRender { [weak self] in self?.bridge?.setEnableFogCulling(v) } } }
  }
  var enforceCulledScreenSpaceError: Bool? {
    didSet { if let v = enforceCulledScreenSpaceError { postToRender { [weak self] in self?.bridge?.setEnforceCulledScreenSpaceError(v) } } }
  }
  var culledScreenSpaceError: Double? {
    didSet { if let v = culledScreenSpaceError { postToRender { [weak self] in self?.bridge?.setCulledScreenSpaceError(v) } } }
  }
  var enableLodTransitionPeriod: Bool? {
    didSet { if let v = enableLodTransitionPeriod { postToRender { [weak self] in self?.bridge?.setEnableLodTransitionPeriod(v) } } }
  }
  var lodTransitionLength: Double? {
    didSet { if let v = lodTransitionLength { postToRender { [weak self] in self?.bridge?.setLodTransitionLength(v) } } }
  }
  var sqliteCacheMaxRows: Double? {
    didSet { if let v = sqliteCacheMaxRows { postToRender { [weak self] in self?.bridge?.setSqliteCacheMaxRows(Int32(v)) } } }
  }
  var taskProcessorThreads: Double? {
    didSet { if let v = taskProcessorThreads { postToRender { [weak self] in self?.bridge?.setTaskProcessorThreads(Int32(v)) } } }
  }

  // MARK: - Render-thread dispatch helper

  /// Posts a closure to the dedicated render queue. Used by every prop setter
  /// that mutates engine state, so the EngineConfig + GPU resources are only
  /// ever touched from one thread (renderQueue) — the render path itself, the
  /// surface init, and bridge teardown all run there.
  private func postToRender(_ action: @escaping () -> Void) {
    renderQueue.async { action() }
  }

  var onMetrics: ((CesiumMetrics) -> Void)?

  // MARK: - Methods

  func getCameraState() throws -> Promise<CameraState> {
    guard let b = bridge else {
      return Promise.rejected(
        withError: NSError(
          domain: "HybridCesiumView",
          code: 1,
          userInfo: [NSLocalizedDescriptionKey: "Native bridge not initialized"]
        )
      )
    }
    let s = CameraState(
      latitude: b.readCameraLatitude(),
      longitude: b.readCameraLongitude(),
      altitude: b.readCameraAltitude(),
      heading: b.readCameraHeading(),
      pitch: b.readCameraPitch(),
      roll: b.readCameraRoll(),
      verticalFovDeg: b.readVerticalFovDeg()
    )
    return Promise.resolved(withResult: s)
  }

  func setCamera(camera: CameraState) throws {
    // Runtime camera updates flow through this method. We do *not* mutate
    // `initialCamera` (it is construction-time only). If the view is not
    // ready yet, the value is buffered in `pendingRuntimeCamera` and pushed
    // during initialization.
    pendingRuntimeCamera = camera
    pendingRuntimeViewCorrection = nil
    postToRender { [weak self] in self?.pushCameraStateIfChanged(camera) }
  }

  func setCameraQuaternion(camera: CameraState, viewCorrection: Quaternion) throws {
    pendingRuntimeCamera = camera
    pendingRuntimeViewCorrection = viewCorrection
    postToRender { [weak self] in self?.pushCameraQuaternionIfChanged(camera, viewCorrection: viewCorrection) }
  }

  func getViewCorrection() throws -> Promise<Quaternion> {
    guard let b = bridge else {
      return Promise.resolved(
        withResult: Quaternion(w: 1, x: 0, y: 0, z: 0)
      )
    }
    let q = Quaternion(
      w: b.readViewCorrectionW(),
      x: b.readViewCorrectionX(),
      y: b.readViewCorrectionY(),
      z: b.readViewCorrectionZ()
    )
    return Promise.resolved(withResult: q)
  }

  // MARK: - View

  private let metalView: MTKView
  private var bridge: CesiumBridge?
  private var displayLink: CADisplayLink?
  private var layoutPollTimer: Timer?
  private var metricsFrameCounter: Int = 0
  private var idleProbeAccumulator: Double = 0
  private var usingLowRefreshRate = false
  private var hasConfiguredFrameRate = false
  private var lastPushedCameraState: CameraState?
  /// Last quaternion pushed via `setCameraQuaternion`; `nil` if only `setCamera` has been used.
  private var lastPushedViewCorrection: Quaternion?
  /// Camera state requested by `setCamera` before the bridge was ready.
  private var pendingRuntimeCamera: CameraState?
  private var pendingRuntimeViewCorrection: Quaternion?
  private var lastBridgePixelSize: CGSize = .zero

  /// Dedicated serial render queue. CADisplayLink fires on `.main`, but the
  /// actual `bridge.shouldRenderNextFrame()` / `bridge.renderFrame(...)` work
  /// is dispatched here so the main thread is never blocked on Cesium tile
  /// loading or Metal command-buffer encoding. All bridge prop setters also
  /// dispatch here so they serialize with rendering and never race with
  /// engine state.
  private let renderQueue = DispatchQueue(
    label: "com.margelo.cesium.render",
    qos: .userInteractive
  )
  /// Drops a CADisplayLink tick if the render queue is still finishing the
  /// previous frame — keeps it from queuing up an unbounded backlog under
  /// stutter (Metal's frameSemaphore would already throttle, but this saves
  /// the dispatch hop and keeps `dt` accurate).
  private var renderInFlight = false
  private let renderInFlightLock = NSLock()

  var view: UIView { metalView }

  override init() {
    let device = MTLCreateSystemDefaultDevice()!
    metalView = MTKView(frame: .zero, device: device)
    metalView.colorPixelFormat = .bgra8Unorm_srgb
    metalView.depthStencilPixelFormat = .depth32Float
    metalView.autoResizeDrawable = false
    metalView.isPaused = true
    metalView.enableSetNeedsDisplay = false
    metalView.isMultipleTouchEnabled = true

    super.init()

    metalView.layer.isOpaque = true
  }

  // MARK: - Lifecycle

  func beforeUpdate() {}

  func afterUpdate() {
    if bridge == nil && layoutPollTimer == nil {
      layoutPollTimer = Timer.scheduledTimer(withTimeInterval: 1.0 / 60.0, repeats: true) { [weak self] _ in
        guard let self = self else { return }
        if self.metalView.bounds.width > 0 {
          self.layoutPollTimer?.invalidate()
          self.layoutPollTimer = nil
          self.ensureInitialized()
        }
      }
    }
    ensureInitialized()
  }

  private func ensureInitialized() {
    guard bridge == nil,
          let metalLayer = metalView.layer as? CAMetalLayer,
          metalView.bounds.width > 0 else { return }

    let scale = metalView.contentScaleFactor
    let w = Int(metalView.bounds.width * scale)
    let h = Int(metalView.bounds.height * scale)
    let pixelSize = CGSize(width: w, height: h)
    metalView.drawableSize = pixelSize
    lastBridgePixelSize = pixelSize

    let cacheDir = NSSearchPathForDirectoriesInDomains(
      .cachesDirectory, .userDomainMask, true
    ).first ?? NSTemporaryDirectory()

    bridge = CesiumBridge(
      metalLayer: metalLayer,
      width: Int32(w),
      height: Int32(h),
      cacheDir: cacheDir
    )

    syncBridgeOptionsFromProps()

    if !ionAccessToken.isEmpty {
      bridge?.updateIonAccessToken(ionAccessToken, assetId: Int64(ionAssetId))
    }
    // Apply construction-time seed first; if the consumer already invoked
    // setCamera / setCameraQuaternion before we were ready, replay it last so
    // the runtime value wins.
    pushCameraStateIfChanged(initialCamera)
    if let runtime = pendingRuntimeCamera {
      if let q = pendingRuntimeViewCorrection {
        pushCameraQuaternionIfChanged(runtime, viewCorrection: q)
      } else {
        pushCameraStateIfChanged(runtime)
      }
    }
    if ionImageryAssetId != 1 {
      bridge?.updateImageryAssetId(Int64(ionImageryAssetId))
    }

    let sc = Int32(msaaSampleCount)
    bridge?.setMsaaSampleCount(sc)
    applyMtkViewMsaa(Int(sc))

    startRenderLoop()
  }

  private func applyMtkViewMsaa(_ s: Int) {
    guard let d = metalView.device else {
      metalView.sampleCount = 1
      return
    }
    if s >= 4, d.supportsTextureSampleCount(4) {
      metalView.sampleCount = 4
    } else if s >= 2, d.supportsTextureSampleCount(2) {
      metalView.sampleCount = 2
    } else {
      metalView.sampleCount = 1
    }
  }

  private func syncBridgeOptionsFromProps() {
    bridge?.setMaximumScreenSpaceError(maximumScreenSpaceError)
    bridge?.setMaximumSimultaneousTileLoads(Int32(maximumSimultaneousTileLoads))
    bridge?.setLoadingDescendantLimit(Int32(loadingDescendantLimit))
    if let v = maximumCachedMiB { bridge?.setMaximumCachedMiB(Int32(v)) }
    if let v = preloadAncestors { bridge?.setPreloadAncestors(v) }
    if let v = preloadSiblings { bridge?.setPreloadSiblings(v) }
    if let v = forbidHoles { bridge?.setForbidHoles(v) }
    if let v = enableWaterMask { bridge?.setEnableWaterMask(v) }
    if let v = enableFogCulling { bridge?.setEnableFogCulling(v) }
    if let v = enforceCulledScreenSpaceError { bridge?.setEnforceCulledScreenSpaceError(v) }
    if let v = culledScreenSpaceError { bridge?.setCulledScreenSpaceError(v) }
    if let v = enableLodTransitionPeriod { bridge?.setEnableLodTransitionPeriod(v) }
    if let v = lodTransitionLength { bridge?.setLodTransitionLength(v) }
    if let v = sqliteCacheMaxRows { bridge?.setSqliteCacheMaxRows(Int32(v)) }
    if let v = taskProcessorThreads { bridge?.setTaskProcessorThreads(Int32(v)) }
  }

  /// Pushes camera state into native bridge only when values changed.
  private func pushCameraStateIfChanged(_ camera: CameraState) {
    guard bridge != nil else { return }
    if let last = lastPushedCameraState,
       last.latitude == camera.latitude,
       last.longitude == camera.longitude,
       last.altitude == camera.altitude,
       last.heading == camera.heading,
       last.pitch == camera.pitch,
       last.roll == camera.roll,
       last.verticalFovDeg == camera.verticalFovDeg {
      return
    }
    lastPushedCameraState = camera
    bridge?.updateCameraLatitude(
      camera.latitude,
      longitude: camera.longitude,
      altitude: camera.altitude,
      heading: camera.heading,
      pitch: camera.pitch,
      roll: camera.roll
    )
    bridge?.setVerticalFovDeg(camera.verticalFovDeg)
  }

  private func pushCameraQuaternionIfChanged(_ camera: CameraState, viewCorrection: Quaternion) {
    guard bridge != nil else { return }
    if let lastCam = lastPushedCameraState,
       let lastQ = lastPushedViewCorrection,
       lastCam.latitude == camera.latitude,
       lastCam.longitude == camera.longitude,
       lastCam.altitude == camera.altitude,
       lastCam.heading == camera.heading,
       lastCam.pitch == camera.pitch,
       lastCam.roll == camera.roll,
       lastCam.verticalFovDeg == camera.verticalFovDeg,
       lastQ.w == viewCorrection.w,
       lastQ.x == viewCorrection.x,
       lastQ.y == viewCorrection.y,
       lastQ.z == viewCorrection.z {
      return
    }
    lastPushedCameraState = camera
    lastPushedViewCorrection = viewCorrection
    bridge?.updateCameraQuaternionLatitude(
      camera.latitude,
      longitude: camera.longitude,
      altitude: camera.altitude,
      heading: camera.heading,
      pitch: camera.pitch,
      roll: camera.roll,
      viewCorrectionW: viewCorrection.w,
      x: viewCorrection.x,
      y: viewCorrection.y,
      z: viewCorrection.z
    )
    bridge?.setVerticalFovDeg(camera.verticalFovDeg)
  }

  // MARK: - Render Loop

  private func startRenderLoop() {
    displayLink = CADisplayLink(target: self, selector: #selector(renderFrame))
    setDisplayLinkFrameRate(idle: false)
    displayLink?.add(to: .main, forMode: .common)
    displayLink?.isPaused = pauseRendering
  }

  private func setDisplayLinkFrameRate(idle: Bool) {
    guard let dl = displayLink else { return }
    if hasConfiguredFrameRate && usingLowRefreshRate == idle { return }
    hasConfiguredFrameRate = true
    usingLowRefreshRate = idle
    dl.preferredFrameRateRange = idle
      ? CAFrameRateRange(minimum: 5, maximum: 15, preferred: 10)
      : CAFrameRateRange(minimum: 30, maximum: 60, preferred: 60)
  }

  @objc private func renderFrame() {
    guard !pauseRendering,
          let dl = displayLink,
          let bridge else { return }

    let dt = max(dl.targetTimestamp - dl.timestamp, 1.0 / 120.0)

    // metalView.bounds / drawableSize are UIKit, must stay on main.
    let scale = metalView.contentScaleFactor
    let w = Int(metalView.bounds.width * scale)
    let h = Int(metalView.bounds.height * scale)
    var didResize = false
    if w > 0 && h > 0 {
      let pixelSize = CGSize(width: w, height: h)
      if lastBridgePixelSize != pixelSize {
        metalView.drawableSize = pixelSize
        lastBridgePixelSize = pixelSize
        didResize = true
      }
    }

    // Throttle: drop the tick if the render queue hasn't finished the previous
    // frame yet. This keeps the dispatch backlog at zero and lets the next
    // CADisplayLink fire pick up a fresh dt.
    renderInFlightLock.lock()
    if renderInFlight {
      renderInFlightLock.unlock()
      return
    }
    renderInFlight = true
    renderInFlightLock.unlock()

    let resizedW = w
    let resizedH = h
    let weakSelf = self  // captured for main-thread post-frame work
    renderQueue.async { [weak self, weak bridge] in
      guard let self = self, let bridge = bridge else {
        weakSelf.renderInFlightLock.lock()
        weakSelf.renderInFlight = false
        weakSelf.renderInFlightLock.unlock()
        return
      }

      if didResize {
        bridge.resize(Int32(resizedW), height: Int32(resizedH))
        bridge.markNeedsRender()
      }

      let shouldRenderNow = bridge.shouldRenderNextFrame()
      let nextIsIdle: Bool
      if shouldRenderNow {
        self.idleProbeAccumulator = 0
        bridge.renderFrame(withDt: dt)
        nextIsIdle = false
      } else {
        self.idleProbeAccumulator += dt
        if self.idleProbeAccumulator >= 0.25 {
          // Safety probe: occasionally tick engine state for late tile completions.
          self.idleProbeAccumulator = 0
          bridge.markNeedsRender()
        }
        nextIsIdle = true
      }

      self.metricsFrameCounter += 1
      let shouldEmitMetrics =
        self.metricsFrameCounter >= 20 && self.onMetrics != nil
      let metricsSnapshot: CesiumMetrics? = shouldEmitMetrics ? CesiumMetrics(
        fps: bridge.metricsFps,
        tilesRendered: Double(bridge.metricsTilesRendered),
        tilesLoading: Double(bridge.metricsTilesLoading),
        tilesVisited: Double(bridge.metricsTilesVisited),
        ionTokenConfigured: bridge.metricsIonTokenConfigured,
        tlsConfigured: bridge.metricsTlsConfigured,
        tilesetReady: bridge.metricsTilesetReady,
        creditsPlainText: bridge.metricsCreditsPlainText
      ) : nil
      if shouldEmitMetrics { self.metricsFrameCounter = 0 }

      // Hop back to main to update CADisplayLink rate (UIKit) and emit the
      // metrics callback (consumer may touch UI from it).
      DispatchQueue.main.async { [weak self] in
        guard let self = self else { return }
        self.setDisplayLinkFrameRate(idle: nextIsIdle)
        if let m = metricsSnapshot, let cb = self.onMetrics {
          cb(m)
        }
      }

      self.renderInFlightLock.lock()
      self.renderInFlight = false
      self.renderInFlightLock.unlock()
    }
  }

  deinit {
    layoutPollTimer?.invalidate()
    displayLink?.invalidate()

    // Drain the render queue so we don't release `bridge` while it's
    // mid-encode. `sync` here is bounded by Metal's frameSemaphore wait —
    // which itself is already bounded to 1 s in MetalBackend::shutdown().
    let bridgeRef = bridge
    bridge = nil
    if let b = bridgeRef {
      renderQueue.sync {
        b.shutdown()
      }
    }
  }
}
