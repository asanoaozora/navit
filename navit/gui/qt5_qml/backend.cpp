#include <glib.h>

#include "item.h"
#include "attr.h"
#include "navit.h"
#include "xmlconfig.h" // for NAVIT_OBJECT
#include "layout.h"
#include "map.h"
#include "transform.h"
#include "backend.h"

#include "qml_map.h"
#include "qml_poi.h"

#include "mapset.h"


#include <QQmlContext>

extern "C" {
#include "proxy.h"
}


Backend::Backend(QObject * parent):QObject(parent)
{
}

void
 Backend::showMenu(struct point *p)
{
	struct coord co;

	transform_reverse(navit_get_trans(nav), p, &co);
	dbg(lvl_debug, "Point 0x%x 0x%x\n", co.x, co.y);
	dbg(lvl_debug, "Screen coord : %d %d\n", p->x, p->y);
	transform_to_geo(transform_get_projection(navit_get_trans(nav)), &co, &(this->g));
	dbg(lvl_debug, "%f %f\n", this->g.lat, this->g.lng);
	dbg(lvl_debug, "%p %p\n", nav, &c);
	this->c.pro = transform_get_projection(navit_get_trans(nav));
	this->c.x = co.x;
	this->c.y = co.y;
	dbg(lvl_debug, "c : %x %x\n", this->c.x, this->c.y);

	emit displayMenu();
}

void
 Backend::get_maps()
{
	struct attr attr, on, off, description, type, data, active;
	char * label;
	bool is_active;
	struct attr_iter * iter;
    _maps.clear();

	iter = navit_attr_iter_new();
	on.type = off.type = attr_active;
	on.u.num = 1;
	off.u.num = 0;
	while (navit_get_attr(this->nav, attr_map, &attr, iter)) {
		if (map_get_attr(attr.u.map, attr_description, &description, NULL)) {
			label = g_strdup(description.u.str);
		} else {
			if (!map_get_attr(attr.u.map, attr_type, &type, NULL))
				type.u.str = "";
			if (!map_get_attr(attr.u.map, attr_data, &data, NULL))
				data.u.str = "";
			label = g_strdup_printf("%s:%s", type.u.str, data.u.str);
		}
		is_active = false;
		if (map_get_attr(attr.u.map, attr_active, &active, NULL)) {
			if (active.u.num == 1) {
				is_active = true;
			}
		}
		_maps.append(new MapObject(label, is_active));
	}

    emit mapsChanged();
}

void
 Backend::set_navit(struct navit *nav)
{
	this->nav = nav;
}

void
 Backend::set_engine(QQmlApplicationEngine * engine)
{
	this->engine = engine;
}

void
 Backend::get_pois()
{
	struct map_selection * sel, * selm;
	struct coord c, center;
	struct mapset_handle * h;
	struct map * m;
	struct map_rect * mr;
	struct item * item;
	enum projection pro = this->c.pro;
	int idist, dist;
    _pois.clear();

	sel = map_selection_rect_new(&(this->c), dist * transform_scale(abs(this->c.y) + dist * 1.5), 18);
	center.x = this->c.x;
	center.y = this->c.y;

	dbg(lvl_debug, "center is at %x, %x\n", center.x, center.y);

	h = mapset_open(navit_get_mapset(this->nav));
	while ((m = mapset_next(h, 1))) {
		selm = map_selection_dup_pro(sel, pro, map_projection(m));
		mr = map_rect_new(m, selm);
		dbg(lvl_debug, "mr=%p\n", mr);
		if (mr) {
			while ((item = map_rect_get_item(mr))) {
				struct attr attr;
				char * label;
                char * icon = get_icon(this->nav, item);
				idist = transform_distance(pro, &center, &c);
				if (item_attr_get(item, attr_label, &attr)) {
					label = map_convert_string(item->map, attr.u.str);
                    if (icon) {
    					_pois.append(new PoiObject(label, idist, icon));
                    }
				}
			}
			map_rect_destroy(mr);
		}
		map_selection_destroy(selm);
	}
	map_selection_destroy(sel);
	mapset_close(h);
    emit poisChanged();

}

QQmlListProperty<QObject> Backend::getPois(){
    return QQmlListProperty<QObject>(this, _pois);
}

QQmlListProperty<QObject> Backend::getMaps(){
    return QQmlListProperty<QObject>(this, _maps);
}

void Backend::show_poi(int index){
    dbg(lvl_debug, "#%i : %s\n",
        index,
        ((PoiObject *)_pois.at(index))->name().toUtf8().data()
    );
}

QString Backend::get_icon_path(){
    return QString(g_strjoin(NULL,"file://",getenv("NAVIT_SHAREDIR"),"/xpm/",NULL));
}

