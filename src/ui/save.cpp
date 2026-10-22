#include "save.h"

#include <algorithm>

namespace L = rap::layout;

std::shared_ptr<SaveDialog> SaveDialog::open(rap::ui& ui, rap::window* parent, const std::string& own_extension,
                                             std::map<std::string, EncodeOptions>& remembered, Callbacks callbacks)
{
    auto d = std::shared_ptr<SaveDialog>(new SaveDialog);
    d->self_ = d;
    d->ui_ = &ui;
    d->remembered_ = &remembered;
    d->callbacks_ = std::move(callbacks);
    int initial = 0;
    std::vector<std::string> names;
    for (const Encoder* e : encoders())
    {
        if (own_extension == e->extension)
            initial = int(d->pages_.size());
        names.push_back(e->name);
        d->pages_.push_back({e, nullptr, std::vector<Control>(e->settings.size())});
    }
    if (d->pages_.empty())
        return nullptr;

    // Every format's settings are built, and the chosen one's shown. The
    // dialog doesn't resize, so it's tall enough for the format with most.
    size_t most = 1;
    for (const Page& p : d->pages_)
        most = std::max(most, p.encoder->settings.size());
    rap::nodes pages;
    for (Page& p : d->pages_)
        pages.push_back(d->page_node(p));
    SaveDialog* s = d.get();
    d->window_ = ui.create(L::dialog{"Save Image As",
                                     {L::vbox{
                                         // The preview's file size and its toggle sit with the format,
                                         // in the same place whatever the format's settings.
                                         L::form{{"Format", L::hbox{L::drop_down{names, initial}.on_changed([s](int i) { s->show_format(i); }).bind(s->format_),
                                                                    L::label{""}.bind(s->info_).width(rap::extent{8, rap::extent::unit::chars}),
                                                                    L::check{"Preview", true}
                                                                        .on_toggled(
                                                                            [s](bool on)
                                                                            {
                                                                                if (s->callbacks_.preview_shown)
                                                                                    s->callbacks_.preview_shown(on);
                                                                            })
                                                                        .bind(s->preview_)}}},
                                         L::separator{},
                                         rap::node{rap::spec::box{rap::orientation::vertical, pages}, {}},
                                         L::spacer{},
                                         L::buttons{L::ok("_Save…"), L::cancel},
                                     }}}
                               .size(rap::extent{62, rap::extent::unit::chars}, rap::extent{float(5 + 2.2 * double(most)), rap::extent::unit::lines})
                               .modeless()
                               .resizable(false),
                           parent);
    for (Page& p : d->pages_)
        d->update_enabled(p);
    d->window_->on_finished([s](rap::dialog_role role) { s->finish(role == rap::dialog_role::ok); });
    d->show_format(initial);
    d->window_->show();
    return d;
}

rap::node SaveDialog::page_node(Page& page)
{
    const Encoder& e = *page.encoder;
    const EncodeOptions& known = (*remembered_)[e.extension];
    std::vector<L::form_row> rows;
    SaveDialog* s = this;
    Page* p = &page;
    for (size_t i = 0; i < e.settings.size(); ++i)
    {
        const EncodeSetting& set = e.settings[i];
        const int value = known.get(set.key, set.initial);
        Control& c = page.controls[i];
        rap::node beside = L::label{set.note ? set.note : ""};  // a few words
        switch (set.kind)
        {
            case EncodeSetting::Kind::Range:
                rows.push_back({set.label, L::hbox{L::slider{double(set.min), double(set.max), double(value)}
                                                       .step(1, std::max(1, (set.max - set.min) / 10))
                                                       .show_value(0)
                                                       .on_changed([s](double) { s->report(); })
                                                       .bind(c.range)
                                                       .width(rap::extent{26, rap::extent::unit::chars}),
                                                   beside}});
                break;
            case EncodeSetting::Kind::Toggle:
                rows.push_back({"", L::hbox{L::check{set.label, value != 0}
                                                .on_toggled(
                                                    [s, p](bool)
                                                    {
                                                        s->update_enabled(*p);
                                                        s->report();
                                                    })
                                                .bind(c.toggle),
                                            beside}});
                break;
            case EncodeSetting::Kind::Choice:
            {
                std::vector<std::string> choices(set.choices.begin(), set.choices.end());
                rows.push_back({set.label, L::hbox{L::drop_down{choices, value}.on_changed([s](int) { s->report(); }).bind(c.choice), beside}});
                break;
            }
        }
    }
    if (rows.empty())
        rows.push_back({"", L::label{std::string(e.name) + " has no settings."}});
    rap::spec::form form;
    for (L::form_row& r : rows)
    {
        form.labels.push_back(r.label);
        form.fields.push_back(std::move(r.field));
    }
    rap::node node{std::move(form), {}};
    node.common.handle = &page.box;
    return node;
}

void SaveDialog::finish(bool save)
{
    if (finished_)
        return;
    finished_ = true;
    auto keep = self_.lock();  // the callbacks may drop the app's reference
    const int i = format_->current();
    if (save && i >= 0)
    {
        const Page& p = pages_[size_t(i)];
        const EncodeOptions o = options(p);
        (*remembered_)[p.encoder->extension] = o;
        if (callbacks_.chosen)
            callbacks_.chosen(*p.encoder, o);
    }
    if (window_)
        window_->close();
    ui_->post([keep] { keep->window_.reset(); });  // not from inside its own callback
    if (callbacks_.closed)
        callbacks_.closed();
}

EncodeOptions SaveDialog::options(const Page& page) const
{
    EncodeOptions o;
    const Encoder& e = *page.encoder;
    for (size_t i = 0; i < e.settings.size(); ++i)
    {
        const Control& c = page.controls[i];
        int v = e.settings[i].initial;
        if (c.range)
            v = int(c.range->value() + 0.5);
        else if (c.toggle)
            v = c.toggle->checked() ? 1 : 0;
        else if (c.choice)
            v = std::max(0, c.choice->current());
        o.values[e.settings[i].key] = v;
    }
    return o;
}

// A setting that only applies while a toggle is off ("quality" under
// "lossless") is greyed while it's on.
void SaveDialog::update_enabled(Page& page)
{
    const Encoder& e = *page.encoder;
    const EncodeOptions o = options(page);
    for (size_t i = 0; i < e.settings.size(); ++i)
        if (const char* off = e.settings[i].off_when)
        {
            const bool enabled = o.get(off, 0) == 0;
            const Control& c = page.controls[i];
            if (c.range)
                c.range->set_enabled(enabled);
            if (c.choice)
                c.choice->set_enabled(enabled);
            if (c.toggle)
                c.toggle->set_enabled(enabled);
        }
}

void SaveDialog::show_format(int index)
{
    for (size_t i = 0; i < pages_.size(); ++i)
        if (pages_[i].box)
            pages_[i].box->set_visible(int(i) == index);
    report();
}

void SaveDialog::report()
{
    const int i = format_ ? format_->current() : -1;
    if (i < 0 || !callbacks_.changed)
        return;
    const Page& p = pages_[size_t(i)];
    const EncodeOptions o = options(p);
    if (preview_)
        preview_->set_enabled(p.encoder->lossy(o));  // nothing to preview when it's kept exactly
    callbacks_.changed(*p.encoder, o);
}

void SaveDialog::set_info(const std::string& text)
{
    if (info_)
        info_->set_text(text);
}

void SaveDialog::close()
{
    finish(false);
}
