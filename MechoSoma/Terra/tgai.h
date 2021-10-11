#ifndef __TGAI_H__
#define __TGAI_H__

#pragma pack (1)
struct TGAHEAD
{
	char IDLenght;		// 00h ������ ���� ID-����������� // 0
	char ColorMapType;	// 01h 0-������� ���, 1-������� ����
	char ImageType;		// 02h ��� ����������� (02h -true color , 08h-compessed)
	short CMapStart;	// 03h ����� ����� � ������ ���������� �������
	short CMapLenght;	// 05h ���������� ��������� � �������
	char CMapDepth;		// 07h ���������� ����� � �������� �������
	short XOffset;		// 08h ��������� X-���������� �����������
	short YOffset;		// 0Ah ��������� Y-���������� �����������
	short Width;		// 0Ch ������ �����������
	short Height;		// 0Eh ������ �����������
	char PixelDepth;	// 10h ���������� ����� � �������
	char ImageDescriptor;	// 11h (���� 0-3 ���������� ����� ��������� � �������)(4,5 ������� ������ �����������)
							// 20h ������ ����������� � ����� ������� ����
	TGAHEAD(){
		init();
	};
	void init(void){
		IDLenght=0;
		ColorMapType=0;
		ImageType=2;
		CMapStart=0;CMapLenght=0;CMapDepth=0;
		XOffset=0;YOffset=0;
		Width=0;Height=0;
		PixelDepth=24;
		ImageDescriptor=0x20;
	}
	void save3layers(char* fname,int sizeX,int sizeY,unsigned char* Ra,unsigned char* Ga,unsigned char* Ba);
	void load3layers(char* fname,int sizeX,int sizeY,unsigned char* Ra,unsigned char* Ga,unsigned char* Ba);

};

#pragma pack()
#endif	// __TGAI_H__
