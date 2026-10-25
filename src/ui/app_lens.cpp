// Lens corrections from lensfun's database for the raw shown. Part of App (app.h).
#include "i18n.h"
#include "app.h"
#include "lens_database.h"
#include "lens_store.h"
#include "platform.h"
#include "view.h"

#include <cstdio>

namespace
{

// The info panel's line for it, in place of what it said before; whether
// that changed.
bool note(Image& image, const std::string& value)
{
    const char* group = N_("Raw");
    const char* key = N_("Lens correction");
    for (MetadataGroup& g : image.metadata)
        if (g.name == group)
            for (MetadataItem& item : g.items)
                if (item.key == key)
                {
                    if (item.value == value)
                        return false;
                    item.value = value;
                    return true;
                }
    image.add(group, key, value);
    return true;
}

std::string number(float v)
{
    char buf[16];
    std::snprintf(buf, sizeof buf, "%g", v);
    return buf;
}

}  // namespace

void App::set_lens_correction(bool on, bool save)
{
    cfg_.lens_correction = on;
    if (save)
        cfg_.save();
    correct_lens(true);
}

void App::script_lens(bool on)
{
    lens_check_->set_checked(on);  // set from code: no callback
    set_lens_correction(on, false);
}

void App::correct_lens(bool redevelop)
{
    if (!image_ || !image_->is_raw)
        return;
    // Any raw starts the database on its way (read, or fetched), so it's
    // likely in before the develop is.
    lenses::want(&App::lenses_arrived);
    const std::shared_ptr<const RawMosaic> m = image_->mosaic;
    if (!m || !m->shot || m->channels != 1 || m->ycc.block_h || (m->corrections && !m->lensfun))
        return;  // not developed here, not a Bayer mosaic, or it has its own (a DNG's)
    const lenses::Shot& shot = *m->shot;
    std::string what;
    std::shared_ptr<const std::vector<uint32_t>> words;
    const std::shared_ptr<const lenses::Database> db = lenses::database();
    if (!cfg_.lens_correction)
        what = "off";
    else if (!db)
        what = "not applied: " + lenses::state();
    else if (const lenses::Camera* camera = db->camera(shot); !camera)
        what = "not applied: the " + shot.make + " " + shot.model + " isn't in lensfun's database";
    else
    {
        std::string why;
        if (const lenses::Lens* lens = db->lens(*camera, shot, why); !lens)
            what = "not applied: " + why + (shot.lens.empty() ? "" : " (" + shot.lens + ")");
        else
        {
            dng::Corrections c = lenses::corrections(*lens, *camera, shot);
            what = "lensfun: " + lens->models[0] + ", ";
            if (c.words.empty())
                what += "nothing calibrated at " + number(shot.focal) + " mm, f/" + number(shot.aperture);
            else
            {
                what += c.summary;
                words = std::make_shared<const std::vector<uint32_t>>(std::move(c.words));
            }
        }
    }
    if (note(*image_, what))
        platform::trace("lenses: %s", what.c_str());
    const bool same = words ? m->lensfun && m->corrections && *m->corrections == *words : !m->lensfun;
    if (!same)
    {
        auto changed = std::make_shared<RawMosaic>(*m);
        changed->corrections = words;
        changed->lensfun = words != nullptr;
        image_->mosaic = std::move(changed);
        image_->pixels_stale = true;  // any read back were of the develop before
        if (redevelop)
            view_->image_changed();  // developed again, with them or without
    }
    if (redevelop)
        refresh_metadata();
}
