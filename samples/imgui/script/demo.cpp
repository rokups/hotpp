// The script of the sample as a plain C++ file. Demo derives from the host class UiApp
// and overrides Frame. A normal C++ build compiles this file. The tool hotc also translates it.
#include "UiApp.h"
#include "imgui.h"

class Demo : public UiApp
{
public:
    void Frame() override
    {
        if (ImGui::Begin("Demo")) {
            ImGui::Text("Hello from a script");
            ImGui::Text("Clicks: %d", this->clicks);
            if (ImGui::Button("Click me")) {
                this->clicks = this->clicks + 1;
            }
            ImGui::Checkbox("Enabled", &this->on);
            ImGui::SliderFloat("Speed", &this->speed, 0.0f, 10.0f);
        }
        ImGui::End();
    }

    int clicks = 0;
    float speed = 1.0f;
    bool on = false;
};
