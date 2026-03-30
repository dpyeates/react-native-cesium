package com.reactnativecesium;

import com.facebook.react.bridge.NativeModule;
import com.facebook.react.bridge.ReactApplicationContext;
import com.facebook.react.module.model.ReactModuleInfoProvider;
import com.facebook.react.uimanager.ViewManager;
import com.facebook.react.BaseReactPackage;
import com.margelo.nitro.reactnativecesium.ReactNativeCesiumOnLoad;
import com.margelo.nitro.reactnativecesium.views.HybridCesiumViewManager;


public class ReactNativeCesiumPackage : BaseReactPackage() {
  override fun getModule(name: String, reactContext: ReactApplicationContext): NativeModule? = null

  override fun getReactModuleInfoProvider(): ReactModuleInfoProvider = ReactModuleInfoProvider { emptyMap() }

  override fun createViewManagers(reactContext: ReactApplicationContext): List<ViewManager<*, *>> {
    val viewManagers = ArrayList<ViewManager<*, *>>()
    viewManagers.add(HybridCesiumViewManager())
    return viewManagers
  }

  companion object {
    init {
      ReactNativeCesiumOnLoad.initializeNative();
    }
  }
}
