// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "HardwareProbeRunner.h"

#include "GstEngine.h"
#include "Core/Player/Domain/HardwareProbe.h"

#include <gst/gst.h>

#include <QCoreApplication>
#include <QEvent>
#include <QFile>
#include <QSysInfo>

#include <cstring>

namespace nordtile { namespace player {

namespace {

const QEvent::Type kProbeEventType = static_cast<QEvent::Type>(QEvent::registerEventType());
enum ProbeEventKind { kFrame = 1, kError, kEnd };

class ProbeEvent : public QEvent {
public:
    ProbeEvent(quint64 generation, int kind, const std::string &text)
        : QEvent(kProbeEventType), generation(generation), kind(kind), text(text) {}
    quint64 generation;
    int kind;
    std::string text;
};

bool applyProperty(GstElement *element, const PropertySetting &p, std::string *why) {
    GParamSpec *spec = g_object_class_find_property(G_OBJECT_GET_CLASS(element), p.name.c_str());
    if (!spec) {
        if (p.optional) return true;
        *why = std::string(GST_ELEMENT_NAME(element)) + " has no property " + p.name;
        return false;
    }
    if (G_PARAM_SPEC_VALUE_TYPE(spec) == G_TYPE_STRING) {
        g_object_set(element, p.name.c_str(), p.value.c_str(), nullptr);
        return true;
    }
    GValue value = G_VALUE_INIT;
    g_value_init(&value, G_PARAM_SPEC_VALUE_TYPE(spec));
    const bool ok = gst_value_deserialize(&value, p.value.c_str());
    if (ok) g_object_set_property(G_OBJECT(element), p.name.c_str(), &value);
    g_value_unset(&value);
    if (!ok && !p.optional) *why = std::string(GST_ELEMENT_NAME(element)) + ": bad value for " + p.name;
    return ok || p.optional;
}

GstElement *makeElement(const ElementPlan &plan, std::string *why) {
    GstElement *e = gst_element_factory_make(plan.factory.c_str(), plan.name.empty() ? nullptr : plan.name.c_str());
    if (!e) {
        *why = "cannot create " + plan.factory;
        return nullptr;
    }
    for (std::size_t i = 0; i < plan.properties.size(); ++i) {
        if (!applyProperty(e, plan.properties[i], why)) {
            gst_object_unref(e);
            return nullptr;
        }
    }
    return e;
}

std::string readSmallFile(const char *path) {
    QFile f(QString::fromLatin1(path));
    if (!f.open(QIODevice::ReadOnly)) return std::string();
    return f.read(16384).toStdString();
}

} // namespace

// One probe run: everything the streaming threads touch; freed by stop() once the pipeline is NULL.
struct GstHardwareProbeRunner::Run {
    GstHardwareProbeRunner *owner = nullptr;
    quint64 generation = 0;
    GstElement *pipeline = nullptr;
    GstElement *head = nullptr;            // chain front (droidvdec)
    std::vector<std::string> prefixes;
    ElementPlan unmatched;
    gint linked = 0;
    gint framed = 0;

    void post(int kind, const std::string &text = std::string()) {
        QCoreApplication::postEvent(owner, new ProbeEvent(generation, kind, text));
    }

    static GstPadProbeReturn sinkBuffer(GstPad *, GstPadProbeInfo *, gpointer data) {
        Run *r = static_cast<Run *>(data);
        if (g_atomic_int_compare_and_exchange(&r->framed, 0, 1)) r->post(kFrame);
        return GST_PAD_PROBE_REMOVE;
    }

    static GstBusSyncReply busSync(GstBus *, GstMessage *m, gpointer data) {
        Run *r = static_cast<Run *>(data);
        if (GST_MESSAGE_TYPE(m) == GST_MESSAGE_ERROR) {
            GError *error = nullptr;
            gst_message_parse_error(m, &error, nullptr);
            std::string text = GST_MESSAGE_SRC_NAME(m) ? std::string(GST_MESSAGE_SRC_NAME(m)) + ": " : std::string();
            text += error && error->message ? error->message : "error";
            if (error) g_error_free(error);
            r->post(kError, text);
        } else if (GST_MESSAGE_TYPE(m) == GST_MESSAGE_EOS) {
            r->post(kEnd);
        }
        return GST_BUS_DROP;
    }

    static void padAdded(GstElement *source, GstPad *pad, gpointer data) {
        Run *r = static_cast<Run *>(data);
        GstCaps *caps = gst_pad_get_current_caps(pad);
        if (!caps) caps = gst_pad_query_caps(pad, nullptr);
        const char *name = caps && gst_caps_get_size(caps) > 0 ? gst_structure_get_name(gst_caps_get_structure(caps, 0)) : "";
        bool match = false;
        for (std::size_t i = 0; i < r->prefixes.size() && !match; ++i)
            match = std::strncmp(name, r->prefixes[i].c_str(), r->prefixes[i].size()) == 0;
        if (caps) gst_caps_unref(caps);
        if (match && g_atomic_int_compare_and_exchange(&r->linked, 0, 1)) {
            GstPad *sinkPad = gst_element_get_static_pad(r->head, "sink");
            const bool ok = sinkPad && gst_pad_link(pad, sinkPad) == GST_PAD_LINK_OK;
            if (sinkPad) gst_object_unref(sinkPad);
            if (!ok) r->post(kError, "cannot link the parsed video to the decoder");
            return;
        }
        // Anything else (or a second video pad) ends in a fakesink: never not-linked.
        if (r->unmatched.factory.empty()) return;
        ElementPlan plan = r->unmatched;
        plan.name.clear();
        std::string ignored;
        GstElement *sink = makeElement(plan, &ignored);
        if (!sink) return;
        GstElement *parent = GST_ELEMENT(gst_element_get_parent(source));
        if (!parent) { gst_object_unref(sink); return; }
        gst_bin_add(GST_BIN(parent), sink);
        gst_element_sync_state_with_parent(sink);
        GstPad *sinkPad = gst_element_get_static_pad(sink, "sink");
        gst_pad_link(pad, sinkPad);
        gst_object_unref(sinkPad);
        gst_object_unref(parent);
    }
};

GstHardwareProbeRunner::GstHardwareProbeRunner(QObject *parent) : QObject(parent) {}

GstHardwareProbeRunner::~GstHardwareProbeRunner() { stop(); }

bool GstHardwareProbeRunner::elementPresent(const char *factory) {
    if (!GstEngine::waitInitialized(GstEngine::kInitWaitMs)) return false;  // NFR-PERF-01: never race gst_init
    GstElementFactory *f = gst_element_factory_find(factory);
    if (!f) return false;
    gst_object_unref(f);
    return true;
}

std::string GstHardwareProbeRunner::currentBuildKey(const QString &appVersion) {
    GstEngine::waitInitialized(GstEngine::kInitWaitMs);  // NFR-PERF-01; without GStreamer droid = "none"
    const std::string hw = readSmallFile("/etc/hw-release");
    std::string device = releaseFileValue(hw, "NAME");
    if (device.empty()) device = releaseFileValue(hw, "MER_HA_DEVICE");
    if (device.empty()) device = QSysInfo::currentCpuArchitecture().toStdString();
    const std::string os = releaseFileValue(readSmallFile("/etc/os-release"), "VERSION_ID");
    std::string droid = "none";
    if (GstPluginFeature *f = GST_PLUGIN_FEATURE(gst_element_factory_find("droidvdec"))) {
        if (GstPlugin *plugin = gst_plugin_feature_get_plugin(f)) {
            const char *v = gst_plugin_get_version(plugin);
            droid = v ? v : "unknown";
            gst_object_unref(plugin);
        }
        gst_object_unref(f);
    }
    gchar *gst = gst_version_string();
    const std::string key = hardwareProbeBuildKey(device, os + " " + (gst ? gst : ""), droid, appVersion.toStdString());
    g_free(gst);
    return key;
}

bool GstHardwareProbeRunner::start(const std::string &clipUri, const Listener &listener, std::string *why) {
    stop();
    if (!GstEngine::waitInitialized(GstEngine::kInitWaitMs)) {  // NFR-PERF-01: never race gst_init
        *why = "GStreamer is not initialised";
        return false;
    }
    std::set<std::string> available;
    const std::vector<std::string> names = elementsToProbe();
    for (std::size_t i = 0; i < names.size(); ++i)
        if (elementPresent(names[i].c_str())) available.insert(names[i]);
    const PipelinePlan plan = buildProbePlan(ProbeKind::HardwareDecode, clipUri, available);
    if (!plan.ok || plan.branches.size() != 1 || plan.branches[0].routes.size() != 1 ||
        plan.branches[0].routes[0].chain.empty()) {
        std::string text;
        for (std::size_t i = 0; i < plan.errors.size(); ++i) text += (i ? "; " : "") + plan.errors[i];
        *why = text.empty() ? std::string("no probe plan") : text;
        return false;
    }
    const BranchPlan &branch = plan.branches[0];
    const PadRoute &route = branch.routes[0];

    Run *r = new Run;
    r->owner = this;
    r->generation = ++generation_;
    r->prefixes = route.capsPrefixes;
    r->unmatched = plan.unmatchedPadSink;
    r->pipeline = gst_pipeline_new("hardware-probe");
    GstElement *source = makeElement(branch.source, why);
    bool ok = source != nullptr;
    if (ok) gst_bin_add(GST_BIN(r->pipeline), source);
    GstElement *previous = nullptr;
    for (std::size_t i = 0; ok && i < route.chain.size(); ++i) {
        GstElement *e = makeElement(route.chain[i], why);
        if (!e) { ok = false; break; }
        gst_bin_add(GST_BIN(r->pipeline), e);
        if (previous && !gst_element_link(previous, e)) {
            *why = "cannot link " + route.chain[i - 1].factory + " to " + route.chain[i].factory;
            ok = false;
        }
        if (i == 0) r->head = e;
        previous = e;
    }
    if (ok) {
        // The chain's last element is the sink: its first buffer is the first decoded frame.
        GstPad *pad = gst_element_get_static_pad(previous, "sink");
        if (pad) {
            gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, &Run::sinkBuffer, r, nullptr);
            gst_object_unref(pad);
        }
        g_signal_connect(source, "pad-added", G_CALLBACK(&Run::padAdded), r);
        GstBus *bus = gst_element_get_bus(r->pipeline);
        gst_bus_set_sync_handler(bus, &Run::busSync, r, nullptr);
        gst_object_unref(bus);
    }
    run_ = r;
    listener_ = listener;
    if (!ok) {
        stop();
        return false;
    }
    if (gst_element_set_state(r->pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        // The bus error (if any) carries the reason; report the state change itself.
        *why = "the probe pipeline refused to start";
        stop();
        return false;
    }
    return true;
}

void GstHardwareProbeRunner::stop() {
    ++generation_;  // posted events of this run are stale now
    listener_ = Listener();
    Run *r = run_;
    run_ = nullptr;
    if (!r) return;
    if (r->pipeline) {
        gst_element_set_state(r->pipeline, GST_STATE_NULL);  // no streaming thread after this
        GstBus *bus = gst_element_get_bus(r->pipeline);
        gst_bus_set_sync_handler(bus, nullptr, nullptr, nullptr);
        gst_object_unref(bus);
        gst_object_unref(r->pipeline);
    }
    delete r;
}

bool GstHardwareProbeRunner::event(QEvent *e) {
    if (e->type() != kProbeEventType) return QObject::event(e);
    const ProbeEvent *p = static_cast<const ProbeEvent *>(e);
    if (p->generation != generation_ || !run_) return true;
    // Copies: the listener may stop() (and so reset listener_) from inside.
    const Listener l = listener_;
    switch (p->kind) {
    case kFrame:
        if (l.frame) l.frame();
        break;
    case kError:
        if (l.error) l.error(p->text);
        break;
    case kEnd:
        if (!g_atomic_int_get(&run_->framed) && l.error) l.error("the clip ended without a decoded frame");
        break;
    }
    return true;
}

} } // namespace nordtile::player
