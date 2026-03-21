#include <vector>

#ifndef HALO_ENABLE_POPUP_NOTIFICATIONS
#define HALO_ENABLE_POPUP_NOTIFICATIONS 1
#endif

struct NotificationItem {
    String title;
    String text;
    String qrLink;
};

std::vector<NotificationItem> notificationQueue;
bool updateAvailable = false;
String latestVersionString = "";
String update_link = "";

int globalNotificationIndex = 0;
int dailyNotificationsShown = 0;
int lastDaySeen = -1;
unsigned long lastNotificationTime = 0; 
const unsigned long NOTIFICATION_INTERVAL_MS = 2 * 60 * 60 * 1000; // 2 hours, maybe adjust later
const int MAX_DAILY_NOTIFICATIONS = 5;

lv_obj_t * current_notification_msgbox = NULL;


static void close_notification_event_handler(lv_event_t * e) {
    lv_obj_t * mbox = (lv_obj_t*)lv_event_get_target(e);
    lv_obj_t * parent = lv_obj_get_parent(lv_obj_get_parent(mbox)); 
    lv_msgbox_close(parent);
    current_notification_msgbox = NULL;
}


void show_notification_popup(String title, String text, String qrPayload) {
#if !HALO_ENABLE_POPUP_NOTIFICATIONS
    (void)title;
    (void)text;
    (void)qrPayload;
    return;
#endif
    // Prevent stacking
    if (current_notification_msgbox != NULL) {
        lv_msgbox_close(current_notification_msgbox);
        current_notification_msgbox = NULL;
    }

    current_notification_msgbox = lv_msgbox_create(NULL);
    lv_obj_t * btn = lv_msgbox_add_footer_button(current_notification_msgbox, localized_text->close);
    lv_msgbox_add_title(current_notification_msgbox, title.c_str());
    lv_msgbox_add_text(current_notification_msgbox, text.c_str());
    
    lv_obj_t * content_obj = lv_msgbox_get_content(current_notification_msgbox);

    lv_obj_t * qr = lv_qrcode_create(content_obj);
    lv_qrcode_set_size(qr, 100);
    lv_qrcode_update(qr, qrPayload.c_str(), qrPayload.length());

    lv_obj_set_style_border_color(qr, lv_color_white(), 0);
    lv_obj_set_style_border_width(qr, 5, 0);
    
    lv_obj_t * scan_lbl = lv_label_create(content_obj);
    lv_label_set_text(scan_lbl, localized_text->scan_to_open);
    
    lv_obj_set_flex_flow(content_obj, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content_obj, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_add_event_cb(btn, close_notification_event_handler, LV_EVENT_CLICKED, NULL);
    lv_obj_center(current_notification_msgbox);
}


void notification_scheduler_task(lv_timer_t *timer) {
#if !HALO_ENABLE_POPUP_NOTIFICATIONS
    (void)timer;
    return;
#endif
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) return;

    int currentHour = timeinfo.tm_hour;
    int currentDay = timeinfo.tm_mday;

    // Day Reset Logic -- disabled for now (keep rolling count)
    /*if (lastDaySeen != currentDay) {
        dailyNotificationsShown = 0;
        lastDaySeen = currentDay;
    }*/

    // Constraints Check: Between 10am (10) and 8pm (20) and haven't exceeded daily limit
    if (currentHour >= 10 && currentHour < 20 && dailyNotificationsShown < MAX_DAILY_NOTIFICATIONS) {            
        // PRIORITY: UPDATE CHECK
        if (updateAvailable) {
            show_notification_popup(localized_text->update_available_title, 
                            "Version " + latestVersionString + " is out.", 
                            update_link); // remember to change link
            
            dailyNotificationsShown++;
            lastNotificationTime = millis();
            updateAvailable = false; // Only show once per detection
            return; 
        }

        // STANDARD NOTIFICATIONS
        if (notificationQueue.size() > 0) {
            int actualIndex = globalNotificationIndex % notificationQueue.size(); // Wrap around if exceeding available notifications
            
            NotificationItem notification = notificationQueue[actualIndex];
            show_notification_popup(notification.title, notification.text, notification.qrLink);

            globalNotificationIndex++; 
            dailyNotificationsShown++;
            lastNotificationTime = millis();
            
            Serial.printf("Displayed Notification %d of %d\n", actualIndex, notificationQueue.size());
        }
    }
}
