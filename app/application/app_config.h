#ifndef APP_CONFIG_H
#define APP_CONFIG_H

/*****************************************************************************
@brief: Initialize product external components with application-owned configuration
@para:
@return: RET_OK indicates success, other return values indicate failure
*******************************************************************************/
int app_components_init(void);

/*****************************************************************************
@brief: Initialize product service modules with application-owned configuration
@para:
@return: RET_OK indicates success, other return values indicate failure
*******************************************************************************/
int app_services_init(void);





#endif
