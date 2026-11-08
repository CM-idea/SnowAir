#pragma once
#include <windows.h>
#include <vector>
#include <string>

// 极简 PDF 1.4 生成器：每页一张 JPEG，图片铺满页面。
class PdfWriter
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
