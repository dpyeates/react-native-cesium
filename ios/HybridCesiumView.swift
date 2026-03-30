import Foundation
import Metal
import MetalKit
import NitroModules

class HybridCesiumView: HybridCesiumViewSpec {
  // MARK: - Props

  var ionAccessToken: String = "" {
    didSet { bridge?.updateIonAccessToken(ionAccessToken, assetId: Int64(ionAssetId)) }
  }
  var ionAssetId: Double = 1 {
    didSet { bridge?.updateIonAccessToken(ionAccessToken, assetId: Int64(ionAssetId)) }
  }
  // Swiss Valais: aerial default toward Matterhorn
  var cameraLatitude: Double = 46.15 {
    didSet { pushCameraParams() }
  }
  var cameraLongitude: Double = 7.35 {
    didSet { pushCameraParams() }
  }
  var cameraAltitude: Double = 12_000 {
    didSet { pushCameraParams() }
  }
  var cameraHeading: Double = 129 {
    didSet { pushCameraParams() }
  }
  var cameraPitch: Double = -45 {
    didSet { pushCameraParams() }
  }
  var cameraRoll: Double = 0 {
    didSet { pushCameraParams() }
  }
  var debugOverlay: Bool = false {
    didSet { bridge?.setDebugOverlay(debugOverlay) }
  }
  var ionImageryAssetId: Double = 1 {
    didSet { bridge?.updateImageryAssetId(Int64(ionImageryAssetId)) }
  }

  // MARK: - Methods

  func setJoystickRates(pitchRate: Double, rollRate: Double) throws -> Void {
    bridge?.setJoystickPitchRate(pitchRate, rollRate: rollRate)
  }

  func onTouchStart(pointerId: Double, x: Double, y: Double) throws -> Void {
    bridge?.onTouchDown(Int32(pointerId), x: Float(x), y: Float(y))
  }

  func onTouchChange(pointerId: Double, x: Double, y: Double) throws -> Void {
    bridge?.onTouchMove(Int32(pointerId), x: Float(x), y: Float(y))
  }

  func onTouchEnd(pointerId: Double) throws -> Void {
    bridge?.onTouchUp(Int32(pointerId))
  }

  // MARK: - View

  private let metalView: MTKView
  private var bridge: CesiumBridge?
  private var displayLink: CADisplayLink?
  private var layoutPollTimer: Timer?

  var view: UIView { metalView }

  override init() {
    let device = MTLCreateSystemDefaultDevice()!
    metalView = MTKView()
    metalView.device = device
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

    if !ionAccessToken.isEmpty {
      bridge?.updateIonAccessToken(ionAccessToken, assetId: Int64(ionAssetId))
    }
    pushCameraParams()
    if ionImageryAssetId != 1 {
      bridge?.updateImageryAssetId(Int64(ionImageryAssetId))
    }

    startRenderLoop()
    setupGestures()
  }

  private func pushCameraParams() {
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
  }

  @objc private func renderFrame() {
    guard let metalLayer = metalView.layer as? CAMetalLayer else { return }

    let scale = metalView.contentScaleFactor
    let w = Int(metalView.bounds.width * scale)
    let h = Int(metalView.bounds.height * scale)

    if w > 0 && h > 0 {
      let layerSize = metalLayer.drawableSize
      if Int(layerSize.width) != w || Int(layerSize.height) != h {
        bridge?.resize(Int32(w), height: Int32(h))
      }
    }

    bridge?.renderFrame()
  }

  // MARK: - Touch Gestures

  private func setupGestures() {
    metalView.isUserInteractionEnabled = true
  }

  func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent?) {
    for touch in touches {
      let loc = touch.location(in: metalView)
      let id = touch.hash
      bridge?.onTouchDown(Int32(id), x: Float(loc.x), y: Float(loc.y))
    }
  }

  func touchesMoved(_ touches: Set<UITouch>, with event: UIEvent?) {
    for touch in touches {
      let loc = touch.location(in: metalView)
      let id = touch.hash
      bridge?.onTouchMove(Int32(id), x: Float(loc.x), y: Float(loc.y))
    }
  }

  func touchesEnded(_ touches: Set<UITouch>, with event: UIEvent?) {
    for touch in touches {
      let id = touch.hash
      bridge?.onTouchUp(Int32(id))
    }
  }

  func touchesCancelled(_ touches: Set<UITouch>, with event: UIEvent?) {
    for touch in touches {
      let id = touch.hash
      bridge?.onTouchUp(Int32(id))
    }
  }

  deinit {
    layoutPollTimer?.invalidate()
    displayLink?.invalidate()
    bridge?.shutdown()
  }
}
