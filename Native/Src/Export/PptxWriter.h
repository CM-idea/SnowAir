#pragma once
#include <windows.h>
#include <vector>
#include <string>

// 极简 PPTX 生成器：每页一张 JPEG，按首页尺寸建立幻灯片，其余页等比居中。
class PptxWriter
{
public:
    void addPage(std::vector<BYTE> jpeg, int pixelW, int pixelH);
    bool save(const std::wstring& path);
    int pageCount() const { return (int)pages_.size(); }

private:
    struct Page {
        std::vector<BYTE> jpeg;
        int w{ 0 }, h{ 0 };
    };
    std::vector<Page> pages_;
};
