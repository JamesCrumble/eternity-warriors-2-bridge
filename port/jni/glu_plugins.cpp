/*
 * Glu's Java plugins (com.glu.plugins.*) and the third-party SDKs behind them,
 * as the game's C# scripts see them.
 *
 * Everything is inert (see dyn_inert_prefix) except what the start-up reads back:
 * AJTUtil.GetProperty serves the build's own properties - res/raw/properties.dat
 * from the APK, AES-decrypted once by tools/extract_properties.sh into
 * <game dir>/glu_properties.txt.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <map>
#include <string>

#include "dyn.h"
#include "port_env.h"
#include "trace.h"

static std::map<std::string, std::string> g_props;

static void load_properties(const std::string &path)
{
    FILE *f = fopen(path.c_str(), "r");
    if (!f) {
        trace("glu: %s missing - run tools/extract_properties.sh; every property reads as empty", path.c_str());
        return;
    }
    char *line = NULL;
    size_t cap = 0;
    while (getline(&line, &cap, f) > 0) {
        std::string l = line;
        while (!l.empty() && (l.back() == '\n' || l.back() == '\r'))
            l.pop_back();
        size_t eq = l.find('=');
        if (eq == std::string::npos || l[0] == '#')
            continue;
        std::string v = l.substr(eq + 1);
        /* java.util.Properties escapes ':' and '=' with a backslash. */
        std::string out;
        for (size_t i = 0; i < v.size(); i++)
            if (!(v[i] == '\\' && i + 1 < v.size()))
                out += v[i];
            else
                out += v[++i];
        g_props[l.substr(0, eq)] = out;
    }
    free(line);
    fclose(f);
    trace("glu: %zu properties from %s", g_props.size(), path.c_str());
}

void glu_plugins_register(JNIEnv *env, const std::string &game_dir)
{
    (void)env;
    load_properties(game_dir + "/glu_properties.txt");
    /* Disable online Glu Wallet so local hard currency in Saves/savegame.bin is used directly. */
    g_props["gwallet_sku"] = "";

    for (const char *p : {"com/glu/", "com/tapjoy/", "com/playhaven/", "com/sponsorpay/", "com/flurry/",
                          "com/chartboost/", "com/jirbo/adcolony/", "com/helpshift/", "com/amazonaws/",
                          "com/facebook/", "com/google/android/gms/"})
        dyn_inert_prefix(p);

    /*
     * The scalar answers of Glu's AJavaTools family (AJavaTools.cs in
     * Assembly-CSharp-firstpass). Null strings are not harmless here: an absent
     * OBB download plan became a null event name and an exception that took
     * GluonManager.Awake down with it.
     */
    static std::string files = game_dir + "/files";
    static std::string gdir = game_dir;
    auto str = [](const char *cls, const char *sig, const char *v) {
        std::string val = v;
        dyn_method(cls, sig, [val](JNIEnv *e, jobject, const jvalue *) { return JL(e->NewStringUTF(val.c_str())); });
    };
    auto num = [](const char *cls, const char *sig, jvalue v) {
        dyn_method(cls, sig, [v](JNIEnv *, jobject, const jvalue *) { return v; });
    };
    const char *AJT = "com/glu/plugins/AJavaTools", *DI = "com/glu/plugins/AJTDeviceInfo",
               *GI = "com/glu/plugins/AJTGameInfo", *UT = "com/glu/plugins/AJTUtil";
    str(AJT, "getAdvertisingID()Ljava/lang/String;", "00000000-0000-0000-0000-000000000000");
    num(AJT, "getAdvertisingIDOptOutFlag()Z", JZ(true));
    num(DI, "IsDeviceRooted()Z", JZ(false));
    num(DI, "IsExternalStorageMounted()Z", JZ(true));
    num(DI, "IsGluDebug()Z", JZ(false));
    num(DI, "GetScreenDiagonalInches()D", JD(7.0));
    num(DI, "GetDeviceSDKVersion()I", JI(19));
    num(DI, "GetScreenWidth()I", JI((jint)port_getenv_long("WIDTH", 1280)));
    num(DI, "GetScreenHeight()I", JI((jint)port_getenv_long("HEIGHT", 720)));
    num(DI, "GetScreenLayout()I", JI(3));   /* SCREENLAYOUT_SIZE_LARGE */
    num(DI, "GetFreeSpace()J", JJ(8ll << 30));
    str(DI, "GetAccountEmail()Ljava/lang/String;", "");
    str(DI, "GetAndroidID()Ljava/lang/String;", "5ad0a6ca11b0a4d1");
    str(DI, "GetDeviceCountry()Ljava/lang/String;", "US");
    str(DI, "GetExternalStorageDirectory()Ljava/lang/String;", gdir.c_str());
    num(GI, "GetVersionCode()I", JI(435));
    str(GI, "GetVersionName()Ljava/lang/String;", "4.3.1");
    str(GI, "GetPackageName()Ljava/lang/String;", "com.glu.ewarriors2");
    str(GI, "GetFilesPath()Ljava/lang/String;", files.c_str());
    str(GI, "GetExternalFilesPath()Ljava/lang/String;", files.c_str());
    num(UT, "IsDataRestored()Z", JZ(false));
    num(UT, "LaunchGame()Z", JZ(false));
    num(UT, "GetRunCount()I", JI(2));
    num(UT, "GetRunCountThisVersion()I", JI(2));
    str(UT, "GetOBBDownloadPlan()Ljava/lang/String;", "old");   /* the OBB was already there */
    str(UT, "consumeLaunchUriString()Ljava/lang/String;", "");
    /* Google Play Games: 1 = ConnectionResult.SERVICE_MISSING. The inert 0 read as
     * SUCCESS, and GluonManager then waited for a Google sign-in that never comes
     * instead of logging in to the (stand-in) Gluon server. */
    num("com/glu/plugins/asocial/playgameservices/PlayGameServicesGlu", "IsPlayGameServicesAvailable()I", JI(1));
    num("com/glu/plugins/PlayGameServicesGlu", "IsPlayGameServicesAvailable()I", JI(1));
    str("com/glu/plugins/PlayGameServicesGlu", "GetCurrentPlayerID()Ljava/lang/String;", "LocalHero");
    str("com/glu/plugins/AJTUI", "GetString(Ljava/lang/String;[Ljava/lang/String;)Ljava/lang/String;", "");
    str("com/glu/plugins/aads/gifting/Gifting", "getCurrentCountryISO()Ljava/lang/String;", "US");

    /* Native alert dialogs: logged until the port draws them. Args: callback object,
     * callback method, title, message, buttons... */
    dyn_method("com/glu/plugins/AJTUI", "ShowAlert", [](JNIEnv *, jobject, const jvalue *a) {
        trace("glu: alert [%s.%s] \"%s\": %s  [%s] [%s] [%s]", dyn_str(a[0].l), dyn_str(a[1].l), dyn_str(a[2].l),
              dyn_str(a[3].l), dyn_str(a[4].l), dyn_str(a[5].l), dyn_str(a[6].l));
        return JV();
    });
    dyn_method("com/glu/plugins/AJTDeviceInfo", "GetDeviceLanguage()Ljava/lang/String;",
               [](JNIEnv *e, jobject, const jvalue *) { return JL(e->NewStringUTF("en")); });
    dyn_method("com/glu/plugins/AJTUtil", "GetProperty(Ljava/lang/String;)Ljava/lang/String;",
               [](JNIEnv *e, jobject, const jvalue *a) {
        auto it = g_props.find(dyn_str(a[0].l));
        if (it == g_props.end())
            return JL(NULL);
        return JL(e->NewStringUTF(it->second.c_str()));
    });
}
