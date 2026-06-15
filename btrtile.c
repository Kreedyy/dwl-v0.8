/* ************************************************************************** */
/*                                                 @@@            @@@@@@@@    */
/*                                                  @@@          @@@@@@@@@@   */
/*                                                   @@!         @@!   @@@@   */
/*                                                    !@!        !@!  @!@!@   */
/*   btrtile.c                                         @!!       @!@ @! !@!   */
/*                                                      !!!      !@!!!  !!!   */
/*   By: julmajustus <julmajustus@tutanota.com>          !!:     !!:!   !!!   */
/*                                                        ::!    :!:    !:!   */
/*   Created: 2024/12/15 00:26:07 by julmajustus           ::    ::::::: ::   */
/*   Updated: 2026/05/20 22:51:54 by julmajustus            : :   : : :  :    */
/*                                                                            */
/* ************************************************************************** */

typedef struct LayoutNode {
	unsigned int is_client_node;
	unsigned int is_split_vertically;
	float split_ratio;
	struct LayoutNode *left;
	struct LayoutNode *right;
	struct LayoutNode *split_node;
	struct wlr_box area; /* pixel rect this node currently occupies */
	Client *client;
} LayoutNode;

static void apply_layout(Monitor *m, LayoutNode *node,
						struct wlr_box area, unsigned int is_root);
static void btrtile(Monitor *m);
static LayoutNode *create_client_node(Client *c);
static LayoutNode *create_split_node(unsigned int is_split_vertically,
									LayoutNode *left, LayoutNode *right);
static void destroy_node(LayoutNode *node);
static void destroy_tree(Monitor *m);
static LayoutNode *find_client_node(LayoutNode *node, Client *c);
static LayoutNode *find_suitable_split(Monitor *m, LayoutNode *start,
                                       unsigned int need_vertical, int focused_on_left);
static void init_tree(Monitor *m);
static void insert_client(Monitor *m, Client *focused_client, Client *new_client);
static LayoutNode *remove_client_node(LayoutNode *node, Client *c);
static void remove_client(Monitor *m, Client *c);
static void setratio_h(const Arg *arg);
static void setratio_v(const Arg *arg);
static void swapclients(const Arg *arg);
static unsigned int visible_count(LayoutNode *node, Monitor *m);
static Client *xytoclient(double x, double y);

static double resize_last_update_x, resize_last_update_y;
static uint32_t last_resize_time = 0;
/* Which edge the current mouse-resize gesture is dragging. Decided once when
 * the resize starts (from the cursor's position within the grabbed window) so
 * the dragged border keeps following the cursor instead of flipping to the
 * opposite side when the cursor reverses direction. */
static int resize_drag_right = 1, resize_drag_bottom = 1;

void
apply_layout(Monitor *m, LayoutNode *node,
             struct wlr_box area, unsigned int is_root)
{
	Client *c;
	float ratio;
	unsigned int left_count, right_count, mid;
	struct wlr_box left_area, right_area;

	if (!node)
		return;

	/* Remember the pixel rect this node occupies so mouse resizing can
	 * convert a cursor delta into a split-ratio delta. */
	node->area = area;

	/* If this node is a client node, check if it is visible. */
	if (node->is_client_node) {
		c = node->client;
		if (!c || !VISIBLEON(c, m) || c->isfloating || c->isfullscreen)
			return;
		if (area.x == c->old_geom.x && area.y == c->old_geom.y &&
				area.width == c->old_geom.width && area.height == c->old_geom.height)
			return;
		resize(c, area, 0);
		c->old_geom = area;
		return;
	}

	/* For a split node, we see how many visible children are on each side: */
	left_count  = visible_count(node->left, m);
	right_count = visible_count(node->right, m);

	if (left_count == 0 && right_count == 0) {
		return;
	} else if (left_count > 0 && right_count == 0) {
		apply_layout(m, node->left, area, 0);
		return;
	} else if (left_count == 0 && right_count > 0) {
		apply_layout(m, node->right, area, 0);
		return;
	}

	/* If we’re here, we have visible clients in both subtrees. */
	ratio = node->split_ratio;
	if (ratio < 0.05f)
		ratio = 0.05f;
	if (ratio > 0.95f)
		ratio = 0.95f;

	memset(&left_area, 0, sizeof(left_area));
	memset(&right_area, 0, sizeof(right_area));

	if (node->is_split_vertically) {
		mid = (unsigned int)(area.width * ratio);
		left_area.x      = area.x;
		left_area.y      = area.y;
		left_area.width  = mid;
		left_area.height = area.height;

		right_area.x      = area.x + mid;
		right_area.y      = area.y;
		right_area.width  = area.width  - mid;
		right_area.height = area.height;
	} else {
		/* horizontal split */
		mid = (unsigned int)(area.height * ratio);
		left_area.x     = area.x;
		left_area.y     = area.y;
		left_area.width = area.width;
		left_area.height = mid;

		right_area.x     = area.x;
		right_area.y     = area.y + mid;
		right_area.width = area.width;
		right_area.height= area.height - mid;
	}

	apply_layout(m, node->left,  left_area,  0);
	apply_layout(m, node->right, right_area, 0);
}

void
btrtile(Monitor *m)
{
	Client *c, *focused = NULL;
	int n = 0;
	LayoutNode *found;
	struct wlr_box full_area;

	if (!m)
		return;

	/* Remove non tiled clients from tree. */
	wl_list_for_each(c, &clients, link) {
		if (c->mon == m && !c->isfloating && !c->isfullscreen) {
		} else {
			remove_client(m, c);
		}
	}

	/* If no client is found under cursor, fallback to focustop(m) */
	if (!(focused = xytoclient(cursor->x, cursor->y)))
		focused = focustop(m);

	/* Insert visible clients that are not part of the tree. */
	wl_list_for_each(c, &clients, link) {
		if (VISIBLEON(c, m) && !c->isfloating && !c->isfullscreen && c->mon == m) {
			found = find_client_node(m->root, c);
			if (!found) {
				insert_client(m, focused, c);
			}
			n++;
		}
	}

	if (n == 0)
		return;

	full_area = m->w;
	apply_layout(m, m->root, full_area, 1);
}

LayoutNode *
create_client_node(Client *c)
{
	LayoutNode *node = calloc(1, sizeof(LayoutNode));

	if (!node)
		return NULL;
	node->is_client_node = 1;
	node->split_ratio = 0.5f;
	node->client = c;
	return node;
}

LayoutNode *
create_split_node(unsigned int is_split_vertically,
				LayoutNode *left, LayoutNode *right)
{
	LayoutNode *node = calloc(1, sizeof(LayoutNode));

	if (!node)
		return NULL;
	node->is_client_node = 0;
	node->split_ratio = 0.5f;
	node->is_split_vertically = is_split_vertically;
	node->left = left;
	node->right = right;
	if (left)
		left->split_node = node;
	if (right)
		right->split_node = node;
	return node;
}

void
destroy_node(LayoutNode *node)
{
	if (!node)
		return;
	if (!node->is_client_node) {
		destroy_node(node->left);
		destroy_node(node->right);
	}
	free(node);
}

void
destroy_tree(Monitor *m)
{
	if (!m || !m->root)
		return;
	destroy_node(m->root);
	m->root = NULL;
}

LayoutNode *
find_client_node(LayoutNode *node, Client *c)
{
	LayoutNode *res;

	if (!node || !c)
		return NULL;
	if (node->is_client_node) {
		return (node->client == c) ? node : NULL;
	}
	res = find_client_node(node->left, c);
	return res ? res : find_client_node(node->right, c);
}

LayoutNode *
find_suitable_split(Monitor *m, LayoutNode *start_node,
		unsigned int need_vertical, int focused_on_left)
{
	LayoutNode *n = start_node, *child = NULL;

	if (!m)
		return NULL;

	if (n && n->is_client_node) {
		child = n;
		n = n->split_node;
	}

	while (n) {
		if (!n->is_client_node && n->is_split_vertically == need_vertical
				&& visible_count(n->left, m) > 0
				&& visible_count(n->right, m) > 0) {
			if ((focused_on_left && n->left == child) ||
			    (!focused_on_left && n->right == child))
				return n;
		}
		child = n;
		n = n->split_node;
	}
	return NULL;
}
void
init_tree(Monitor *m)
{
    if (m)
       m->root = NULL;
}

void
insert_client(Monitor *m, Client *focused_client, Client *new_client)
{
	Client *old_client;
	LayoutNode **root = &m->root, *old_root,
	*focused_node, *new_client_node, *old_client_node;
	unsigned int wider, mid_x, mid_y;

	/* If no root , new client becomes the root. */
	if (!*root) {
		*root = create_client_node(new_client);
		return;
	}

	/* Find the focused_client node,
	 * if not found split the root. */
	focused_node = focused_client ?
		find_client_node(*root, focused_client) : NULL;
	if (!focused_node) {
		old_root = *root;
		new_client_node = create_client_node(new_client);
		*root = create_split_node(1, old_root, new_client_node);
		return;
	}

	/* Turn focused node from a client node into a split node,
	 * and attach old_client + new_client. */
	old_client = focused_node->client;
	old_client_node = create_client_node(old_client);
	new_client_node = create_client_node(new_client);

	/* Decide split direction. */
	wider = (focused_client->geom.width >= focused_client->geom.height);
	focused_node->is_client_node = 0;
	focused_node->client         = NULL;
	focused_node->is_split_vertically = (wider ? 1 : 0);

	/* Pick new_client side depending on the cursor position. */
	mid_x = focused_client->geom.x + focused_client->geom.width / 2;
	mid_y = focused_client->geom.y + focused_client->geom.height / 2;

	if (wider) {
		/* vertical split => left vs right */
		if (cursor->x <= mid_x) {
			focused_node->left  = new_client_node;
			focused_node->right = old_client_node;
		} else {
			focused_node->left  = old_client_node;
			focused_node->right = new_client_node;
		}
	} else {
		/* horizontal split => top vs bottom */
		if (cursor->y <= mid_y) {
			focused_node->left  = new_client_node;
			focused_node->right = old_client_node;
		} else {
			focused_node->left  = old_client_node;
			focused_node->right = new_client_node;
		}
	}
	old_client_node->split_node = focused_node;
	new_client_node->split_node = focused_node;
	focused_node->split_ratio = 0.5f;
}

LayoutNode *
remove_client_node(LayoutNode *node, Client *c)
{
	LayoutNode *tmp;
	if (!node)
		return NULL;
	if (node->is_client_node) {
		/* If this client_node is the client we're removing,
		 * return NULL to remove it */
		if (node->client == c) {
			free(node);
			return NULL;
		}
		return node;
	}

	node->left = remove_client_node(node->left, c);
	node->right = remove_client_node(node->right, c);

	/* If one of the client node is NULL after removal and the other is not,
	 * we "lift" the other client node up to replace this split node. */
	if (!node->left && node->right) {
		tmp = node->right;

		/* Save pointer to split node */
		if (tmp)
			tmp->split_node = node->split_node;

		free(node);
		return tmp;
	}

	if (!node->right && node->left) {
		tmp = node->left;

		/* Save pointer to split node */
		if (tmp)
			tmp->split_node = node->split_node;

		free(node);
		return tmp;
	}

	/* If both children exist or both are NULL (empty tree),
	 * return node as is. */
	return node;
}

void
remove_client(Monitor *m, Client *c)
{
	if (!m->root || !c)
		return;
	m->root = remove_client_node(m->root, c);
}

static void
setratio(unsigned int need_vertical, const Arg *arg)
{
	Client *sel;
	LayoutNode *client_node, *split_node;
	float new_ratio;
    int focused_on_left;

	if (!selmon || !selmon->lt[selmon->sellt]->arrange)
		return;

	sel = focustop(selmon);
	if (!sel)
		return;

	client_node = find_client_node(selmon->root, sel);
	if (!client_node)
		return;

	focused_on_left = (arg->f >= 0.0f);

	split_node = find_suitable_split(selmon, client_node, need_vertical, focused_on_left);

	if (!split_node)
		split_node = find_suitable_split(selmon, client_node, need_vertical, !focused_on_left);
	if (!split_node)
		return;

	new_ratio = (arg->f != 0.0f) ? (split_node->split_ratio + arg->f) : 0.5f;
	if (new_ratio < 0.05f)
		new_ratio = 0.05f;
	if (new_ratio > 0.95f)
		new_ratio = 0.95f;
	split_node->split_ratio = new_ratio;

	apply_layout(selmon, selmon->root, selmon->w, 1);
	/* Skip the arrange when called from motionnotify; that path calls
	 * arrange itself after rate-limiting. */
}

/* After the controlling split's ratio changed, keep every window that is *not*
 * adjacent to the dragged edge at its current pixel size, so only the two
 * windows that actually touch the edge resize. Without this, when the
 * controlling split is an ancestor whose subtree on either side holds more than
 * the one adjacent window (e.g. four windows in a row where the focused one is
 * in the middle), changing that split's ratio scales the whole group and drags
 * non-adjacent windows along.
 *
 * This walks one subtree (the region on one side of the dragged divider) toward
 * the divider. divider_high tells which side of the subtree faces the divider:
 * 1 = its right/bottom edge, 0 = its left/top edge. At each split on the same
 * axis as the resize, the child *away* from the divider is pinned to its
 * previous pixel extent and the divider-facing child absorbs the whole change.
 * Splits on the perpendicular axis are recursed into on both sides: every
 * window stacked in the column/row that meets the divider should resize
 * together, so both children still face the divider with the full extent.
 * new_extent is the subtree's pixel extent (width for a vertical resize, height
 * otherwise) after the controlling split's new ratio. */
static void
compensate(LayoutNode *node, unsigned int need_vertical,
           int divider_high, int new_extent)
{
	LayoutNode *adj, *far;
	int far_extent;
	float r;

	while (node && !node->is_client_node) {
		if (node->is_split_vertically == need_vertical) {
			adj = divider_high ? node->right : node->left;
			far = divider_high ? node->left  : node->right;
			far_extent = need_vertical ? far->area.width : far->area.height;
			/* Nothing sensible to pin if the far child has no size or would
			 * leave no room for the adjacent window. */
			if (far_extent <= 0 || far_extent >= new_extent)
				return;
			/* split_ratio is the left/top child's fraction of the extent. */
			r = divider_high ? (float)far_extent / (float)new_extent
			                 : (float)(new_extent - far_extent) / (float)new_extent;
			if (r < 0.05f)
				r = 0.05f;
			if (r > 0.95f)
				r = 0.95f;
			node->split_ratio = r;
			new_extent -= far_extent;
			node = adj;
		} else {
			/* Perpendicular split: both children meet the divider with the full
			 * extent, so compensate each independently. */
			compensate(node->left,  need_vertical, divider_high, new_extent);
			compensate(node->right, need_vertical, divider_high, new_extent);
			return;
		}
	}
}

/* Mouse-drag resize for a single axis: interpret the cursor movement as pixels
 * and convert it to a split-ratio delta using the pixel extent of the split
 * region along the resize axis. This keeps the border tracking the cursor 1:1
 * regardless of monitor resolution. resize_factor scales sensitivity. The
 * caller is responsible for arranging once both axes have been applied.
 *
 * need_vertical selects the axis: 1 adjusts a vertical split (left|right
 * border, driven by horizontal cursor motion), 0 adjusts a horizontal split
 * (top/bottom border, driven by vertical cursor motion). */
void
setratio_px(unsigned int need_vertical, float px_delta)
{
	Client *sel;
	LayoutNode *client_node, *split_node, *near, *far;
	float new_ratio, delta_ratio;
	int focused_on_left, f_left_of_split, extent, near_extent, far_extent;

	if (!selmon || !selmon->lt[selmon->sellt]->arrange || px_delta == 0.0f)
		return;

	sel = focustop(selmon);
	if (!sel)
		return;

	client_node = find_client_node(selmon->root, sel);
	if (!client_node)
		return;

	/* The dragged edge is fixed for the whole gesture. Dragging the right
	 * (or bottom) edge means the focused window is the left (or top) child of
	 * the divider, so growing that split's ratio moves the border outward. */
	focused_on_left = need_vertical ? resize_drag_right : resize_drag_bottom;

	split_node = find_suitable_split(selmon, client_node, need_vertical, focused_on_left);
	if (!split_node)
		split_node = find_suitable_split(selmon, client_node, need_vertical, !focused_on_left);
	if (!split_node)
		return;

	extent = need_vertical ? split_node->area.width : split_node->area.height;
	if (extent <= 0)
		return;

	delta_ratio = (px_delta * resize_factor) / (float)extent;

	new_ratio = split_node->split_ratio + delta_ratio;
	if (new_ratio < 0.05f)
		new_ratio = 0.05f;
	if (new_ratio > 0.95f)
		new_ratio = 0.95f;
	split_node->split_ratio = new_ratio;

	/* Pin every non-adjacent window so only the two windows touching the
	 * dragged edge resize. The controlling split's divider lies on that edge;
	 * compensate both of its subtrees toward the divider. The near subtree
	 * (holding the focused window) faces the divider on the side toward the
	 * focused window; the far subtree faces it from the opposite side. */
	f_left_of_split = find_client_node(split_node->left, sel) != NULL;
	near = f_left_of_split ? split_node->left : split_node->right;
	far  = f_left_of_split ? split_node->right : split_node->left;
	near_extent = (int)(extent * (f_left_of_split ? new_ratio : 1.0f - new_ratio));
	far_extent  = extent - near_extent;
	if (near_extent > 0)
		compensate(near, need_vertical, f_left_of_split, near_extent);
	if (far_extent > 0)
		compensate(far, need_vertical, !f_left_of_split, far_extent);
}

void
setratio_h(const Arg *arg)
{
	setratio(1, arg);
}

void
setratio_v(const Arg *arg)
{
	setratio(0, arg);
}

void swapclients(const Arg *arg) {
	Client  *c, *tmp, *target = NULL, *sel = focustop(selmon);
	LayoutNode *sel_node, *target_node;
	int closest_dist = INT_MAX, dist, sel_center_x, sel_center_y,
	cand_center_x, cand_center_y;

	if (!sel || sel->isfullscreen ||
			!selmon->root || !selmon->lt[selmon->sellt]->arrange)
		return;


	/* Get the center coordinates of the selected client */
	sel_center_x = sel->geom.x + sel->geom.width / 2;
	sel_center_y = sel->geom.y + sel->geom.height / 2;

	wl_list_for_each(c, &clients, link) {
		if (!VISIBLEON(c, selmon) || c->isfloating || c->isfullscreen || c == sel)
			continue;

		/* Get the center of candidate client */
		cand_center_x = c->geom.x + c->geom.width / 2;
		cand_center_y = c->geom.y + c->geom.height / 2;

		/* Check that the candidate lies in the requested direction. */
		switch (arg->ui) {
			case 0:
				if (cand_center_x >= sel_center_x)
					continue;
				break;
			case 1:
				if (cand_center_x <= sel_center_x)
					continue;
				break;
			case 2:
				if (cand_center_y >= sel_center_y)
					continue;
				break;
			case 3:
				if (cand_center_y <= sel_center_y)
					continue;
				break;
			default:
				continue;
		}

		/* Get distance between the centers */
		dist = abs(sel_center_x - cand_center_x) + abs(sel_center_y - cand_center_y);
		if (dist < closest_dist) {
			closest_dist = dist;
			target = c;
		}
	}

	/* If target is found, swap the two clients’ positions in the layout tree */
	if (target) {
		sel_node = find_client_node(selmon->root, sel);
		target_node = find_client_node(selmon->root, target);
		if (sel_node && target_node) {
			tmp = sel_node->client;
			sel_node->client = target_node->client;
			target_node->client = tmp;
			arrange(selmon);
		}
	}
}

unsigned int
visible_count(LayoutNode *node, Monitor *m)
{
	Client *c;

	if (!node)
		return 0;
	/* Check if this client is visible. */
	if (node->is_client_node) {
		c = node->client;
		if (c && VISIBLEON(c, m) && !c->isfloating && !c->isfullscreen)
			return 1;
		return 0;
	}
	/* Else it’s a split node. */
	return visible_count(node->left, m) + visible_count(node->right, m);
}

Client *
xytoclient(double x, double y) {
	Monitor *m = xytomon(x, y);
	Client *c, *closest = NULL;
	double dist, mindist = INT_MAX, dx, dy;

	wl_list_for_each_reverse(c, &clients, link) {
		if (VISIBLEON(c, m) && !c->isfloating && !c->isfullscreen &&
			x >= c->geom.x && x <= (c->geom.x + c->geom.width) &&
			y >= c->geom.y && y <= (c->geom.y + c->geom.height)){
			return c;
		}
	}

	/* If no client was found at cursor position fallback to closest. */
	wl_list_for_each_reverse(c, &clients, link) {
		if (VISIBLEON(c, m) && !c->isfloating && !c->isfullscreen) {
			dx = 0, dy = 0;

			if (x < c->geom.x)
				dx = c->geom.x - x;
			else if (x > (c->geom.x + c->geom.width))
				dx = x - (c->geom.x + c->geom.width);

			if (y < c->geom.y)
				dy = c->geom.y - y;
			else if (y > (c->geom.y + c->geom.height))
				dy = y - (c->geom.y + c->geom.height);

			dist = dx * dx + dy * dy;
			if (dist < mindist) {
				mindist = dist;
				closest = c;
			}
		}
	}
	return closest;
}
