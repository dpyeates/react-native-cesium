import Foundation
import Metal
import MetalKit
import NitroModules
import UIKit

class HybridCesiumView: HybridCesiumViewSpec {
  // MARK: - Props

  var ionAccessToken: String = "" {
    didSet { bridge?.updateIonAccessToken(ionAccessToken, assetId: Int64(ionAssetId)) }
  }
  var ionAssetId: Double = 1 {
    didSet { bridge?.updateIonAccessToken(ionAccessToken, assetId: Int64(ionAssetId)) }
  }

  /// Construction-time seed camera. Once the bridge exists, use `setCamera(_:)`
  /// for runtime updates; later prop writes are stored but do not drive the
  /// active native camera.
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
      displayLink?.isPaused = pauseRendering
      if !pauseRendering {
        bridge?.markNeedsRender()
      }
    }
  }

  var maximumScreenSpaceError: Double = 32 {
    didSet { bridge?.setMaximumScreenSpaceError(maximumScreenSpaceError) }
  }
  var maximumSimultaneousTileLoads: Double = 12 {
    didSet { bridge?.setMaximumSimultaneousTileLoads(Int32(maximumSimultaneousTileLoads)) }
  }
  var loadingDescendantLimit: Double = 20 {
    didSet { bridge?.setLoadingDescendantLimit(Int32(loadingDescendantLimit)) }
  }
  var msaaSampleCount: Double = 1 {
    didSet {
      let s = Int32(msaaSampleCount)
      bridge?.setMsaaSampleCount(s)
      applyMtkViewMsaa(Int(s))
    }
  }
  var ionImageryAssetId: Double = 1 {
    didSet { bridge?.updateImageryAssetId(Int64(ionImageryAssetId)) }
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
    // Runtime camera updates flow through this method; if the view is not ready
    // yet we keep the latest value and apply it during initialization.
    initialCamera = camera
    pushCameraStateIfChanged(camera)
  }

  func setCameraQuaternion(camera: CameraState, viewCorrection: Quaternion) throws {
    initialCamera = camera
    pushCameraQuaternionIfChanged(camera, viewCorrection: viewCorrection)
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
  private var lastBridgePixelSize: CGSize = .zero

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
    pushCameraStateIfChanged(initialCamera)
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

    let scale = metalView.contentScaleFactor
    let w = Int(metalView.bounds.width * scale)
    let h = Int(metalView.bounds.height * scale)

    if w > 0 && h > 0 {
      let pixelSize = CGSize(width: w, height: h)
      if lastBridgePixelSize != pixelSize {
        metalView.drawableSize = pixelSize
        lastBridgePixelSize = pixelSize
        bridge.resize(Int32(w), height: Int32(h))
        bridge.markNeedsRender()
      }
    }

    let shouldRenderNow = bridge.shouldRenderNextFrame()
    if shouldRenderNow {
      setDisplayLinkFrameRate(idle: false)
      idleProbeAccumulator = 0
      bridge.renderFrame(withDt: dt)
    } else {
      setDisplayLinkFrameRate(idle: true)
      idleProbeAccumulator += dt
      if idleProbeAccumulator >= 0.25 {
        // Safety probe: occasionally tick engine state for late tile completions.
        idleProbeAccumulator = 0
        bridge.markNeedsRender()
      }
    }

    metricsFrameCounter += 1
    if metricsFrameCounter >= 20, let cb = onMetrics {
      metricsFrameCounter = 0
      let m = CesiumMetrics(
        fps: bridge.metricsFps,
        tilesRendered: Double(bridge.metricsTilesRendered),
        tilesLoading: Double(bridge.metricsTilesLoading),
        tilesVisited: Double(bridge.metricsTilesVisited),
        ionTokenConfigured: bridge.metricsIonTokenConfigured,
        tilesetReady: bridge.metricsTilesetReady,
        creditsPlainText: bridge.metricsCreditsPlainText
      )
      cb(m)
    }
  }

  deinit {
    layoutPollTimer?.invalidate()
    displayLink?.invalidate()
    bridge?.shutdown()
  }
}
