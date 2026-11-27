package dev.fastsim.dustbound;

import android.app.Activity;
import android.content.ActivityNotFoundException;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.webkit.ValueCallback;
import android.webkit.WebChromeClient;
import android.webkit.WebResourceRequest;
import android.webkit.WebView;
import android.webkit.WebViewClient;

public final class MainActivity extends Activity {
    private static final int PICK_MOD = 1;
    private ValueCallback<Uri[]> modPicker;

    @Override public void onCreate(Bundle saved) {
        super.onCreate(saved);
        WebView game = new WebView(this);
        game.getSettings().setJavaScriptEnabled(true);
        game.getSettings().setDomStorageEnabled(true);
        game.getSettings().setAllowFileAccess(false);
        game.getSettings().setAllowContentAccess(true);
        game.setWebViewClient(new WebViewClient() {
            @Override public boolean shouldOverrideUrlLoading(WebView view, WebResourceRequest request) {
                return true;
            }
        });
        game.setWebChromeClient(new WebChromeClient() {
            @Override public boolean onShowFileChooser(WebView view, ValueCallback<Uri[]> callback, FileChooserParams parameters) {
                if (modPicker != null) modPicker.onReceiveValue(null);
                modPicker = callback;
                Intent intent = new Intent(Intent.ACTION_GET_CONTENT);
                intent.setType("*/*");
                intent.addCategory(Intent.CATEGORY_OPENABLE);
                try {
                    startActivityForResult(intent, PICK_MOD);
                } catch (ActivityNotFoundException e) {
                    callback.onReceiveValue(null);
                    modPicker = null;
                }
                return true;
            }
        });
        setContentView(game);
        game.loadUrl("file:///android_asset/index.html");
    }

    @Override protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (request != PICK_MOD || modPicker == null) return;
        modPicker.onReceiveValue(WebChromeClient.FileChooserParams.parseResult(result, data));
        modPicker = null;
    }
}
