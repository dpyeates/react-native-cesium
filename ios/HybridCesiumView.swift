import Foundation
import Metal
import MetalKit
import NitroModules
import QuartzCore
import UIKit

/// Weak proxy for CADisplayLink so the timer does not strongly retain its
/// owning view. Without this, CADisplayLink → HybridCesiumView → displayLink
/// forms a cycle that prevents `deinit` from ever firing, which in turn
/// prevents the C++ engine and SqliteCache handle from being released.
private final class DisplayLinkProxy {
  weak var owner: HybridCesiumView?
  @objc func tick() { owner?.handleDisplayLinkTick() }
}

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

  /// Construction-time seed camera. Used exactly once via `teleport(...)`
  /// when the native bridge is created; subsequent prop writes are ignored.
  /// Use the per-DoF setters (or `teleport(...)`) for runtime updates.
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
  var minAltitudeAboveTerrain: Double? {
    didSet { if let v = minAltitudeAboveTerrain { postToRender { [weak self] in self?.bridge?.setMinAltitudeAboveTerrain(Float(v)) } } }
  }

  // ── Rate caps ────────────────────────────────────────────────────────────
  var maxYawRateDegSec: Double?   { didSet { pushRateCaps() } }
  var maxPitchRateDegSec: Double? { didSet { pushRateCaps() } }
  var maxRollRateDegSec: Double?  { didSet { pushRateCaps() } }
  var maxClimbRateMps: Double?    { didSet { pushRateCaps() } }
  var maxGroundSpeedMps: Double?  { didSet { pushRateCaps() } }

  private func pushRateCaps() {
    let yaw   = maxYawRateDegSec ?? 0
    let pitch = maxPitchRateDegSec ?? 0
    let roll  = maxRollRateDegSec ?? 0
    let climb = maxClimbRateMps ?? 0
    let gnd   = maxGroundSpeedMps ?? 0
    postToRender { [weak self] in
      self?.bridge?.setRateCapsYaw(yaw, pitch: pitch, roll: roll,
                                   climb: climb, groundSpeed: gnd)
    }
  }

  // MARK: - Render-thread dispatch helper

  /// Posts a closure to the dedicated render queue. Used by every prop setter
  /// that mutates engine state. Camera demand setters do **not** hop here —
  /// the CameraIntegrator's mutex makes per-DoF writes safe from any thread,
  /// and skipping the dispatch keeps latency minimal for high-rate updates.
  private func postToRender(_ action: @escaping () -> Void) {
    renderQueue.async { action() }
  }

  var onMetrics: ((CesiumMetrics) -> Void)?

  // MARK: - Methods

  func getActualCamera() throws -> Promise<CameraState> {
    guard let b = bridge else {
      return Promise.rejected(
        withError: NSError(
          domain: "HybridCesiumView",
          code: 1,
          userInfo: [NSLocalizedDescriptionKey: "Native bridge not initialized"]
        )
      )
    }
    return Promise.resolved(
      withResult: CameraState(
        latitude: b.readActualLatitude(),
        longitude: b.readActualLongitude(),
        altitude: b.readActualAltitude(),
        heading: b.readActualHeading(),
        pitch: b.readActualPitch(),
        roll: b.readActualRoll(),
        verticalFovDeg: b.readActualVerticalFovDeg()
      )
    )
  }

  func getDemandCamera() throws -> Promise<CameraState> {
    guard let b = bridge else {
      return Promise.rejected(
        withError: NSError(
          domain: "HybridCesiumView",
          code: 1,
          userInfo: [NSLocalizedDescriptionKey: "Native bridge not initialized"]
        )
      )
    }
    return Promise.resolved(
      withResult: CameraState(
        latitude: b.readDemandLatitude(),
        longitude: b.readDemandLongitude(),
        altitude: b.readDemandAltitude(),
        heading: b.readDemandHeading(),
        pitch: b.readDemandPitch(),
        roll: b.readDemandRoll(),
        verticalFovDeg: b.readDemandVerticalFovDeg()
      )
    )
  }

  func setPosition(latitude: Double, longitude: Double) throws {
    if let b = bridge {
      b.setPositionLatitude(latitude, longitude: longitude)
    } else {
      pendingLat = latitude
      pendingLon = longitude
    }
  }

  func setAltitude(altitudeMeters: Double) throws {
    if let b = bridge { b.setAltitude(altitudeMeters) }
    else              { pendingAltitude = altitudeMeters }
  }

  func setHeading(headingDeg: Double) throws {
    if let b = bridge { b.setHeadingDeg(headingDeg) }
    else              { pendingHeading = headingDeg }
  }

  func setAttitude(pitchDeg: Double, rollDeg: Double) throws {
    if let b = bridge { b.setAttitudePitch(pitchDeg, roll: rollDeg) }
    else              { pendingPitch = pitchDeg; pendingRoll = rollDeg }
  }

  func setViewCorrection(q: Quaternion) throws {
    if let b = bridge { b.setViewCorrectionW(q.w, x: q.x, y: q.y, z: q.z) }
    else              { pendingViewCorrection = q }
  }

  func setVerticalFov(deg: Double) throws {
    if let b = bridge { b.setVerticalFovDeg(deg) }
    else              { pendingVfov = deg }
  }

  func teleport(camera: CameraState) throws {
    if let b = bridge {
      b.teleportLatitude(camera.latitude,
                         longitude: camera.longitude,
                         altitude: camera.altitude,
                         heading: camera.heading,
                         pitch: camera.pitch,
                         roll: camera.roll,
                         verticalFovDeg: camera.verticalFovDeg)
    } else {
      pendingTeleport = camera
    }
  }

  func getViewCorrection() throws -> Promise<Quaternion> {
    guard let b = bridge else {
      return Promise.resolved(withResult: Quaternion(w: 1, x: 0, y: 0, z: 0))
    }
    return Promise.resolved(
      withResult: Quaternion(
        w: b.readViewCorrectionW(),
        x: b.readViewCorrectionX(),
        y: b.readViewCorrectionY(),
        z: b.readViewCorrectionZ()
      )
    )
  }

  // MARK: - View

  private let metalView: MTKView
  private var bridge: CesiumBridge?
  private var displayLink: CADisplayLink?
  private let displayLinkProxy = DisplayLinkProxy()
  private var layoutPollTimer: Timer?
  private var metricsFrameCounter: Int = 0
  private var idleProbeAccumulator: Double = 0
  private var usingLowRefreshRate = false
  private var hasConfiguredFrameRate = false
  private var lastBridgePixelSize: CGSize = .zero

  /// Demand values that arrived before the bridge was created. Drained once
  /// in `ensureInitialized()` after the seed teleport.
  private var pendingTeleport: CameraState?
  private var pendingLat: Double?
  private var pendingLon: Double?
  private var pendingAltitude: Double?
  private var pendingHeading: Double?
  private var pendingPitch: Double?
  private var pendingRoll: Double?
  private var pendingVfov: Double?
  private var pendingViewCorrection: Quaternion?

  /// Dedicated serial render queue. CADisplayLink fires on `.main`, but the
  /// actual `bridge.shouldRenderNextFrame()` / `bridge.renderFrame(...)` work
  /// is dispatched here so the main thread is never blocked on Cesium tile
  /// loading or Metal command-buffer encoding. All bridge prop setters also
  /// dispatch here so they serialize with rendering and never race with
  /// engine state. Per-DoF camera writes deliberately bypass this hop —
  /// CameraIntegrator's internal mutex handles cross-thread safety.
  private let renderQueue = DispatchQueue(
    label: "com.margelo.cesium.render",
    qos: .userInteractive
  )
  /// Drops a CADisplayLink tick if the render queue is still finishing the
  /// previous frame.
  private var renderInFlight = false
  private let renderInFlightLock = NSLock()

  /// Set to true at the start of `deinit`. Any work still queued on the
  /// render or main queues checks this before touching the bridge or
  /// invoking JS callbacks. Avoids two classes of warnings during fast
  /// refresh / unmount: Nitro "Dispatcher has already been destroyed"
  /// when emitting `onMetrics` into a dying JS runtime, and stray frame
  /// ticks running after the bridge has been torn down.
  private var isShutdown = false

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
    // Seed from initialCamera, then replay anything the consumer set before
    // we were ready.
    let seed = pendingTeleport ?? initialCamera
    bridge?.teleportLatitude(seed.latitude,
                             longitude: seed.longitude,
                             altitude: seed.altitude,
                             heading: seed.heading,
                             pitch: seed.pitch,
                             roll: seed.roll,
                             verticalFovDeg: seed.verticalFovDeg)
    pendingTeleport = nil

    if let la = pendingLat, let lo = pendingLon {
      bridge?.setPositionLatitude(la, longitude: lo); pendingLat = nil; pendingLon = nil
    }
    if let a = pendingAltitude       { bridge?.setAltitude(a);                           pendingAltitude = nil }
    if let h = pendingHeading        { bridge?.setHeadingDeg(h);                         pendingHeading  = nil }
    if let p = pendingPitch, let r = pendingRoll {
      bridge?.setAttitudePitch(p, roll: r); pendingPitch = nil; pendingRoll = nil
    }
    if let f = pendingVfov           { bridge?.setVerticalFovDeg(f);                     pendingVfov     = nil }
    if let q = pendingViewCorrection { bridge?.setViewCorrectionW(q.w, x: q.x, y: q.y, z: q.z); pendingViewCorrection = nil }

    if ionImageryAssetId != 1 {
      bridge?.updateImageryAssetId(Int64(ionImageryAssetId))
    }

    let sc = Int32(msaaSampleCount)
    bridge?.setMsaaSampleCount(sc)
    applyMtkViewMsaa(Int(sc))

    pushRateCaps()

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
    if let v = minAltitudeAboveTerrain { bridge?.setMinAltitudeAboveTerrain(Float(v)) }
  }

  // MARK: - Render Loop

  private func startRenderLoop() {
    displayLinkProxy.owner = self
    displayLink = CADisplayLink(
      target: displayLinkProxy,
      selector: #selector(DisplayLinkProxy.tick)
    )
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

  /// Invoked by `DisplayLinkProxy` on every CADisplayLink tick (main thread).
  fileprivate func handleDisplayLinkTick() {
    guard !pauseRendering,
          !isShutdown,
          let bridge else { return }

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

    // Drop the tick if the render queue hasn't finished the previous frame yet.
    renderInFlightLock.lock()
    if renderInFlight {
      renderInFlightLock.unlock()
      return
    }
    renderInFlight = true
    renderInFlightLock.unlock()

    // Wall-clock time used for the metrics-only frame-dt EMA inside the
    // bridge. The camera integrator owns its own clock — see
    // CameraIntegrator::step().
    let nowSeconds = CACurrentMediaTime()

    let resizedW = w
    let resizedH = h
    renderQueue.async { [weak self, weak bridge] in
      guard let self = self else { return }
      // `renderInFlight` must always be cleared so a single dropped tick
      // doesn't permanently wedge the render loop. defer guarantees it
      // regardless of which early-return path we take below.
      defer {
        self.renderInFlightLock.lock()
        self.renderInFlight = false
        self.renderInFlightLock.unlock()
      }
      guard let bridge = bridge else { return }

      if didResize {
        bridge.resize(Int32(resizedW), height: Int32(resizedH))
        bridge.markNeedsRender()
      }

      let shouldRenderNow = bridge.shouldRenderNextFrame()
      let nextIsIdle: Bool
      if shouldRenderNow {
        self.idleProbeAccumulator = 0
        bridge.renderFrame(at: nowSeconds)
        nextIsIdle = false
      } else {
        // Approximate dt for the idle probe. We don't need precision here.
        let probeDt = 1.0 / 60.0
        self.idleProbeAccumulator += probeDt
        if self.idleProbeAccumulator >= 0.25 {
          self.idleProbeAccumulator = 0
          bridge.markNeedsRender()
        }
        nextIsIdle = true
      }

      self.metricsFrameCounter += 1
      let shouldEmitMetrics =
        self.metricsFrameCounter >= 20 && self.onMetrics != nil && !self.isShutdown
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

      DispatchQueue.main.async { [weak self] in
        guard let self = self, !self.isShutdown else { return }
        self.setDisplayLinkFrameRate(idle: nextIsIdle)
        if let m = metricsSnapshot, let cb = self.onMetrics {
          cb(m)
        }
      }
      // renderInFlight is cleared by the defer at the top of this closure.
    }
  }

  deinit {
    // Set the shutdown gate first so any in-flight render-queue tick and
    // any queued main-queue follow-on (metric emission, frame-rate update)
    // bail out before they touch the bridge or invoke onMetrics into a
    // JS dispatcher that may already be gone.
    isShutdown = true
    onMetrics = nil

    layoutPollTimer?.invalidate()
    layoutPollTimer = nil
    displayLink?.invalidate()
    displayLink = nil
    displayLinkProxy.owner = nil

    let bridgeRef = bridge
    bridge = nil
    if let b = bridgeRef {
      // Drain anything already on the render queue, then ask the bridge
      // to release its engine + Metal backend synchronously. We do this
      // inside the sync hop so the SqliteCache file handle is closed
      // before any subsequent CesiumView mount can open the same cache.
      renderQueue.sync {
        b.shutdown()
      }
    }
  }
}
