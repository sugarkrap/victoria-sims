#ifndef VICTORIA_UI_LAYOUT_READER_HEADER
#define VICTORIA_UI_LAYOUT_READER_HEADER

#include "victoria/coreTypes.h"

#define UI_LAYOUT_TYPE_IDENTIFIER 0x00000000UL
#define UI_LAYOUT_GROUP_IDENTIFIER 0xA99D8A11UL

#define UI_LAYOUT_CLASS_NAME_LIMIT 32UL
#define UI_LAYOUT_CAPTION_LIMIT 96UL
#define UI_LAYOUT_ELEMENT_LIMIT 128U
#define UI_LAYOUT_NESTING_LIMIT 16U

typedef struct UIElement
{
    char className[UI_LAYOUT_CLASS_NAME_LIMIT];

    Boolean hasID;
    Unsigned32 id;

    Integer32 areaLeft;
    Integer32 areaTop;
    Integer32 areaRight;
    Integer32 areaBottom;

    Boolean hasFillColor;
    Unsigned8 fillRed;
    Unsigned8 fillGreen;
    Unsigned8 fillBlue;

    Boolean noFill;

    Boolean hasCaption;
    char caption[UI_LAYOUT_CAPTION_LIMIT];

    Boolean hasImage;
    Unsigned32 imageNumberCount;
    Unsigned32 imageNumbers[3];

    Boolean visible;
    Boolean enabled;

    Integer32 parentIndex;
} UIElement;

typedef enum UILayoutReadResult
{
    UI_LAYOUT_READ_OK = 0,
    UI_LAYOUT_READ_NO_ELEMENTS,
    UI_LAYOUT_READ_UNBALANCED_CHILDREN,
    UI_LAYOUT_READ_TOO_DEEPLY_NESTED
} UILayoutReadResult;

const char *uiLayoutReadResultGetName(UILayoutReadResult result);

typedef struct UILayoutDescription
{
    Unsigned32 elementCount;
    Unsigned32 elementsOverflow;
    UIElement elements[UI_LAYOUT_ELEMENT_LIMIT];
} UILayoutDescription;

UILayoutReadResult uiLayoutRead(UILayoutDescription *description, const Unsigned8 *bytes,
                                MemorySize sizeInBytes);

void uiLayoutGetAbsoluteArea(const UILayoutDescription *description, Unsigned32 elementIndex,
                             Integer32 *left, Integer32 *top, Integer32 *right, Integer32 *bottom);

Boolean uiLayoutIsVisible(const UILayoutDescription *description, Unsigned32 elementIndex);

#endif
