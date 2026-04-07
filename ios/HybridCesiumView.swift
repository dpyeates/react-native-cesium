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

  var cameraLatitude: Double = 46.15 {
    didSet { pushFullCameraFromProps() }
  }
  var cameraLongitude: Double = 7.35 {
    didSet { pushFullCameraFromProps() }
  }
  var cameraAltitude: Double = 12_000 {
    didSet { pushFullCameraFromProps() }
  }
  var cameraHeading: Double = 129 {
    didSet { pushFullCameraFromProps() }
  }
  var cameraPitch: Double = -45 {
    didSet { pushFullCameraFromProps() }
  }
  var cameraRoll: Double = 0 {
    didSet { pushFullCameraFromProps() }
  }

  var cameraVerticalFovDeg: Double = 60 {
    didSet { bridge?.setVerticalFovDeg(cameraVerticalFovDeg) }
  }
  var debugOverlay: Bool = false {
    didSet { bridge?.setDebugOverlay(debugOverlay) }
  }
  var showCredits: Bool = true {
    didSet { bridge?.setShowCredits(showCredits) }
  }
  var pauseRendering: Bool = false {
    didSet { displayLink?.isPaused = pauseRendering }
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

  func flyTo(
    latitude: Double,
    longitude: Double,
    altitude: Double,
    heading: Double,
    pitch: Double,
    roll: Double,
    durationSeconds: Double
  ) throws {
    bridge?.fly(
      toLatitude: latitude,
      longitude: longitude,
      altitude: altitude,
      heading: heading,
      pitch: pitch,
      roll: roll,
      durationSeconds: durationSeconds
    )
  }

  func lookAt(
    targetLatitude: Double,
    targetLongitude: Double,
    targetAltitude: Double,
    durationSeconds: Double
  ) throws {
    bridge?.look(
      atTargetLatitude: targetLatitude,
      longitude: targetLongitude,
      altitude: targetAltitude,
      durationSeconds: durationSeconds
    )
  }

  // MARK: - View

  private let metalView: MTKView
  private var bridge: CesiumBridge?
  private var displayLink: CADisplayLink?
  private var layoutPollTimer: Timer?
  private var metricsFrameCounter: Int = 0

  var view: UIView { metalView }

  override init() {
    let device = MTLCreateSystemDefaultDevice()!
    metalView = MTKView(frame: .zero, device: device)
    metalView.colorPixelFormat = .bgra8Unorm_srgb
    metalView.depthStencilPixelFormat = .depth32Float
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
    bridge?.setShowCredits(showCredits)
  }

  private func ensureInitialized() {
    guard bridge == nil,
          let metalLayer = metalView.layer as? CAMetalLayer,
          metalView.bounds.width > 0 else { return }

    let scale = metalView.contentScaleFactor
    let w = Int(metalView.bounds.width * scale)
    let h = Int(metalView.bounds.height * scale)

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
    pushFullCameraFromProps()
    bridge?.setVerticalFovDeg(cameraVerticalFovDeg)
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
    bridge?.setDebugOverlay(debugOverlay)
    bridge?.setShowCredits(showCredits)
  }

  /// All 6 DOF from React props → demand target in the native bridge.
  /// The bridge's per-frame smooth follower handles transitions.
  private func pushFullCameraFromProps() {
    bridge?.updateCameraLatitude(
      cameraLatitude,
      longitude: cameraLongitude,
      altitude: cameraAltitude,
      heading: cameraHeading,
      pitch: cameraPitch,
      roll: cameraRoll
    )
  }

  // MARK: - Render Loop

  private func startRenderLoop() {
    displayLink = CADisplayLink(target: self, selector: #selector(renderFrame))
    displayLink?.preferredFrameRateRange = CAFrameRateRange(
      minimum: 30, maximum: 60, preferred: 60
    )
    displayLink?.add(to: .main, forMode: .common)
    displayLink?.isPaused = pauseRendering
  }

  @objc private func renderFrame() {
    guard !pauseRendering,
          let dl = displayLink,
          let metalLayer = metalView.layer as? CAMetalLayer else { return }

    let dt = max(dl.targetTimestamp - dl.timestamp, 1.0 / 120.0)

    let scale = metalView.contentScaleFactor
    let w = Int(metalView.bounds.width * scale)
    let h = Int(metalView.bounds.height * scale)

    if w > 0 && h > 0 {
      let layerSize = metalLayer.drawableSize
      if Int(layerSize.width) != w || Int(layerSize.height) != h {
        bridge?.resize(Int32(w), height: Int32(h))
      }
    }

    bridge?.renderFrame(withDt: dt)

    metricsFrameCounter += 1
    if metricsFrameCounter >= 20, let b = bridge, let cb = onMetrics {
      metricsFrameCounter = 0
      let m = CesiumMetrics(
        fps: b.metricsFps,
        tilesRendered: Double(b.metricsTilesRendered),
        tilesLoading: Double(b.metricsTilesLoading),
        tilesVisited: Double(b.metricsTilesVisited),
        ionTokenConfigured: b.metricsIonTokenConfigured,
        tilesetReady: b.metricsTilesetReady,
        creditsPlainText: showCredits ? b.metricsCreditsPlainText : "",
        terrainHeightBelowCamera: b.metricsTerrainHeight
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
