#include <stdio.h>
#include <esp_log.h>
#include <ssd1306.h>

void app_main(void)
{
    init_ssd1306();

    while(1)
    {   
        // Display only show 16 characters per line when aligned all the way to the left
        ssd1306_print_str(0, 0, "P90 SBTS Project", false);
        ssd1306_print_str(0, 16, "192.168.100.100", false);

        ssd1306_display();
        vTaskDelay(1000 / portTICK_PERIOD_MS);
        
        clear_display();
        ssd1306_print_str(0, 32, "Line 3", false);
        ssd1306_display();
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}